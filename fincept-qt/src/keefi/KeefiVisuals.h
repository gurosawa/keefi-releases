#pragma once
#include "keefi/ProductIdentity.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>
#include <functional>

namespace keefi {
// Item roles read by WorkbenchRowDelegate: the legacy avatar key is now shown
// as the full ticker text, and a price direction (+1/-1)
// that adds ▲/▼ so colour is never the only signal.
constexpr int AvatarRole = Qt::UserRole + 20;
constexpr int ChangeDirectionRole = Qt::UserRole + 21;
// Transient typed badges; these observations never enter a profile or CSV.
constexpr int WatchlistBadgesRole = Qt::UserRole + 22;
constexpr int EvidenceNumbersRole = Qt::UserRole + 23;
// Public display name/code/market only; identity stays in the existing UserRole.
constexpr int ChartInstrumentLabelRole = Qt::UserRole + 24;

inline MovementDirection movementForSign(int sign) {
    return sign > 0 ? MovementDirection::Up : sign < 0 ? MovementDirection::Down : MovementDirection::Flat;
}
inline QString movementDecoratedText(const QString& text, MovementDirection direction) {
    const auto marker = MovementColorPolicy::marker(direction);
    return marker.isEmpty() || text.contains(marker) ? text : marker + QStringLiteral(" ") + text;
}

// Observe the nearest window's policy, including a late parent/policy. Only a
// cached presentation callback runs here; this helper owns no data/source job.
class MovementPresentationBinding final : public QObject {
  public:
    MovementPresentationBinding(QWidget* target, std::function<void()> refresh)
        : QObject(target), target_(target), refresh_(std::move(refresh)) { rebind(); }
  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::ParentChange || event->type() == QEvent::Show) {
            rebind();
            if (refresh_) refresh_();
        } else if (event->type() == QEvent::ChildAdded || event->type() == QEvent::ChildRemoved) scheduleRebind();
        return false;
    }
  private:
    void scheduleRebind() {
        if (scheduled_) return;
        scheduled_ = true;
        QTimer::singleShot(0, this, [this] { scheduled_ = false; rebind(); });
    }
    void rebind() {
        for (const auto& watched : watched_) if (watched) watched->removeEventFilter(this);
        watched_.clear();
        for (QObject* at = target_; at; at = at->parent()) { at->installEventFilter(this); watched_.append(at); }
        auto* next = MovementColorPolicy::forContext(target_);
        if (next == policy_) return;
        disconnect(changed_); disconnect(destroyed_); policy_ = next;
        if (next) {
            changed_ = connect(next, &MovementColorPolicy::changed, this, [this] { if (refresh_) refresh_(); });
            destroyed_ = connect(next, &QObject::destroyed, this, [this] { policy_ = nullptr; scheduleRebind(); if (refresh_) refresh_(); });
        }
        if (refresh_) refresh_();
    }
    QPointer<QWidget> target_;
    QPointer<MovementColorPolicy> policy_;
    QList<QPointer<QObject>> watched_;
    QMetaObject::Connection changed_, destroyed_;
    std::function<void()> refresh_;
    bool scheduled_ = false;
};
inline void bindMovementPresentation(QWidget* target, std::function<void()> refresh) {
    new MovementPresentationBinding(target, std::move(refresh));
}

// Shared presentation helpers: the Crossline mark, legacy badges on screens
// not yet migrated, and text-based workbench rows. No data or behaviour lives here.

// Crossline: a grey diagonal crossed by an amber rising line, inside a tile.
inline void paintCrosslineMark(QPainter& painter, const QRectF& box, bool compact = false) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal s = box.width() / 36.0;
    const auto at = [&](qreal x, qreal y) { return QPointF(box.left() + x * s, box.top() + y * s); };
    if (compact) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(themeColors().accent);
        painter.drawRoundedRect(QRectF(at(1, 1), at(35, 35)), 9 * s, 9 * s);
        painter.setPen(QPen(themeColors().bg, 4 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    } else {
        painter.setPen(QPen(themeColors().accent, 1.5 * s));
        painter.setBrush(themeColors().bg);
        painter.drawRoundedRect(QRectF(at(1, 1), at(35, 35)), 10 * s, 10 * s);
        painter.setPen(QPen(themeColors().text3, 2.4 * s, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(at(10, 25), at(26, 11));
        painter.setPen(QPen(themeColors().accent, 2.6 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }
    painter.drawPolyline(QPolygonF{at(10, 11), at(15.5, 16.5), at(18.5, 14), at(26, 25)});
    painter.restore();
}

inline QIcon informationGlyph(const QColor& ink) {
    QPixmap pixels(36,36);
    pixels.setDevicePixelRatio(2);
    pixels.fill(Qt::transparent);
    QPainter painter(&pixels);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(ink,1.5,Qt::SolidLine,Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QRectF(1,1,16,16));
    painter.drawPoint(QPointF(9,5.5));
    painter.drawLine(QPointF(9,8),QPointF(9,12.5));
    painter.end();
    return QIcon(pixels);
}

inline QIcon crosslineIcon() {
    QIcon icon;
    for (const int size : {16, 32, 64, 128}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paintCrosslineMark(painter, QRectF(0, 0, size, size), size <= 16);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

// Categorical public-list flags are separate from financial movement colours.
inline QIcon listFlagIcon(const QString& flag) {
    const auto& c=themeColors();
    const QHash<QString,QColor> inks{{"red",c.downText},{"orange",c.accentText},
        {"yellow",c.flagYellow},{"green",c.upText},{"blue",c.koreanDownText},{"purple",c.studyPurple}};
    if(!inks.contains(flag))return {};
    QIcon result;
    for(const int size:{16,32}){
        QPixmap pixmap(size,size);pixmap.fill(Qt::transparent);QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);painter.scale(size/16.0,size/16.0);
        painter.setPen(QPen(inks.value(flag),1.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(3,2),QPointF(3,14));
        painter.drawPolyline(QPolygonF{{3,3},{13,3},{10,6},{13,9},{3,9}});
        painter.end();result.addPixmap(pixmap);
    }
    return result;
}

class LogoMark final : public QWidget {
  public:
    explicit LogoMark(int size, QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(size, size);
        setObjectName(QStringLiteral("logoMark"));
        setAccessibleName(productName());
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        paintCrosslineMark(painter, QRectF(rect()));
    }
};

struct AvatarTone { QColor background, foreground; };
// Stable muted tone per ticker or name; colour never carries meaning here.
inline AvatarTone avatarTone(const QString& key) {
    const auto& c = themeColors();
    const AvatarTone tones[] = {{c.surface,c.accentText}, {c.surface,c.studyBlue},
        {c.surface,c.studyPurple}, {c.surface,c.upText}, {c.surface,c.downText}, {c.surface,c.text2}};
    uint hash = 0;
    for (const QChar c : key) hash = (hash * 31 + c.unicode()) % 997;
    return tones[hash % (sizeof(tones) / sizeof(tones[0]))];
}
inline QString avatarText(const QString& label) {
    for (const QChar c : label)
        if (c.isLetterOrNumber()) return QString(c).toUpper();
    return QStringLiteral("·");
}
inline void paintAvatar(QPainter& painter, const QRectF& box, const QString& label, qreal radius = -1) {
    const auto tone = avatarTone(label);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(tone.background);
    painter.drawRoundedRect(box, radius < 0 ? box.width() / 2 : radius, radius < 0 ? box.height() / 2 : radius);
    const QFont font = KeefiFonts::font(FontRole::CaptionStrong12);
    painter.setFont(font);
    painter.setPen(tone.foreground);
    painter.drawText(box, Qt::AlignCenter, avatarText(label));
    painter.restore();
}

// Shape numbers/codes and Korean as distinct font runs, including in one cell.
// The model's original text is never reformatted, parsed as a value or mutated.
inline qreal drawLedgerText(QPainter& painter,const QRectF& rect,const QString& text,
                           FontRole bodyRole,Qt::Alignment alignment=Qt::AlignLeft,const QFont* baseFont=nullptr,bool numberRuns=true) {
    if(rect.width()<=0||text.isEmpty())return 0;
    const auto base=baseFont?*baseFont:KeefiFonts::font(bodyRole);
    const auto formats=[numberRuns](const QString& shown){return numberRuns?KeefiFonts::numberRuns(shown):QList<QTextLayout::FormatRange>{};};
    const auto measure=[&](const QString& shown){
        QTextLayout layout(shown,base);layout.setFormats(formats(shown));layout.beginLayout();auto line=layout.createLine();if(line.isValid())line.setLineWidth(1000000);layout.endLayout();return line.isValid()?line.naturalTextWidth():0;
    };
    QString shown=text;
    if(measure(shown)>rect.width()){
        const QString ellipsis=QStringLiteral("…");int low=0,high=text.size();
        while(low<high){const int mid=(low+high+1)/2;if(measure(text.left(mid)+ellipsis)<=rect.width())low=mid;else high=mid-1;}
        if(low>0&&text.at(low-1).isHighSurrogate())--low;
        shown=text.left(low)+ellipsis;if(measure(shown)>rect.width())return 0;
    }
    QTextLayout layout(shown,base);layout.setFormats(formats(shown));layout.beginLayout();auto line=layout.createLine();if(line.isValid())line.setLineWidth(1000000);layout.endLayout();
    if(!line.isValid())return 0;
    const qreal x=alignment.testFlag(Qt::AlignRight)?rect.right()-line.naturalTextWidth():alignment.testFlag(Qt::AlignHCenter)?rect.center().x()-line.naturalTextWidth()/2:rect.left();
    painter.save();painter.setClipRect(rect,Qt::IntersectClip);layout.draw(&painter,QPointF(x,rect.center().y()-line.height()/2));painter.restore();
    return line.naturalTextWidth();
}

// Workbench list rows use text tickers, never initials in a coloured avatar.
// Child rows (per-account holdings) keep their existing model text.
class WorkbenchRowDelegate final : public QStyledItemDelegate {
  public:
    // Keep the existing model role contract; its value is now a text ticker.
    explicit WorkbenchRowDelegate(QObject* parent = nullptr, int avatarRole = AvatarRole)
        : QStyledItemDelegate(parent), avatarRole_(avatarRole) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), index.parent().isValid() ? 34 : 46));
        size.setWidth(size.width() + 24 + (index.data(ChangeDirectionRole).toInt() ? 18 : 0));
        return size;
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const bool selected = item.state & QStyle::State_Selected;
        const bool hover = item.state & QStyle::State_MouseOver;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (selected || hover) {
            painter->setPen(Qt::NoPen);
            // Selection is neutral; amber is kept for actions and navigation.
            painter->setBrush(themeColors().surface);
            painter->drawRect(item.rect);
        }
        QRect text = item.rect.adjusted(12, 0, -12, 0);
        if (index.column() == 0 && !index.parent().isValid()) {
            const auto key = index.data(avatarRole_).toString();
            if (!key.isEmpty() && !item.text.contains(key,Qt::CaseInsensitive)) {
                painter->setFont(KeefiFonts::font(FontRole::Number11));
                painter->setPen(themeColors().text3);
                const int tickerWidth=qMin(painter->fontMetrics().horizontalAdvance(key),text.width()/3);
                const QRect ticker(text.right()-tickerWidth+1,text.top(),tickerWidth,text.height());
                painter->drawText(ticker,Qt::AlignRight|Qt::AlignVCenter,painter->fontMetrics().elidedText(key,Qt::ElideRight,tickerWidth));
                text.setRight(ticker.left()-12);
            }
        }
        // EvidenceNumbersRole remains model data; it is not part of the row.
        const auto bodyRole=index.column() == 0 && !index.parent().isValid()?FontRole::BodyBold15:FontRole::Secondary14;
        const auto foreground = item.palette.color(QPalette::Text);
        const int direction = index.data(ChangeDirectionRole).toInt();
        painter->setPen(direction ? MovementColorPolicy::colorFor(option.widget, movementForSign(direction)) :
            index.parent().isValid() && index.column() == 0 ? themeColors().text3 : foreground);
        const auto alignment = item.displayAlignment & Qt::AlignHorizontal_Mask ? item.displayAlignment : Qt::AlignLeft | Qt::AlignVCenter;
        const auto shown=movementDecoratedText(item.text,movementForSign(direction));
        drawLedgerText(*painter,text,shown,bodyRole,Qt::Alignment(alignment));
        painter->restore();
    }

  private:
    int avatarRole_ = AvatarRole;
};

// A source label remains neutral; each observed badge carries its own typed
// presentation fact. Risk ink never follows the movement scheme.
class WatchlistBadgeDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto value = index.siblingAtColumn(0).data(WatchlistBadgesRole);
        if (!value.isValid()) { QStyledItemDelegate::paint(painter, option, index); return; }
        const auto allBadges = value.value<QList<WatchlistBadge>>();
        QList<WatchlistBadge> badges;
        // The narrow row shows at most two readable states. Keep every state
        // in the model for accessibility/tooltips, prioritizing fixed risk ink.
        for (const auto& badge : allBadges) if (badge.fixedRisk && badges.size() < 2) badges.append(badge);
        for (const auto& badge : allBadges) if (!badge.fixedRisk && badges.size() < 2) badges.append(badge);
        QStyleOptionViewItem item(option); initStyleOption(&item, index);
        // initStyleOption normalizes newlines to line separators for native
        // painting. Keep the model's original rows for this custom layout.
        const auto lines = index.data(Qt::DisplayRole).toString().split('\n'); item.text.clear();
        const bool highlighted = item.state & (QStyle::State_Selected | QStyle::State_MouseOver);
        item.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        const auto* style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, option.widget);
        painter->save(); painter->setClipRect(item.rect);
        QRect rowRect = item.rect;
        const auto* view = qobject_cast<const QAbstractItemView*>(option.widget);
        if (!view && option.widget) view = qobject_cast<const QAbstractItemView*>(option.widget->parentWidget());
        if (view) {
            const auto first = view->visualRect(index.siblingAtColumn(0));
            const auto last = view->visualRect(index.siblingAtColumn(index.model()->columnCount(index.parent()) - 1));
            if (first.isValid() && last.isValid()) rowRect = first.united(last);
        }
        // A narrow name column cannot carry both direction and risk badges.
        // Share the row's bottom line across cells, through each cell's clip,
        // while preserving the fixed 64 px row and the price/change column.
        const bool badgeFooter = !badges.isEmpty() && rowRect.width() > item.rect.width();
        if (highlighted) {
            // Every cell paints the same full-row shape through its own clip,
            // so only the two outer ends have rounded corners.
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen); painter->setBrush(themeColors().surface);
            painter->drawRoundedRect(QRectF(rowRect), 10, 10);
        }
        const auto box = item.rect.adjusted(12,12,-12,-12);
        const qreal firstTop = item.rect.top() + (badgeFooter ? 2 : 12);
        const QRectF first(box.left(),firstTop,box.width(),20),second(box.left(),firstTop+20,box.width(),20);
        QRectF badgeBox = badgeFooter ? QRectF(rowRect.left()+12,rowRect.bottom()-21,rowRect.width()-24,20) : second;
        const auto drawBadges = [&] {
            qreal x=badgeBox.left();const QString separator=QStringLiteral(" · ");
            const qreal separatorWidth=QFontMetricsF(KeefiFonts::font(FontRole::Secondary14)).horizontalAdvance(separator);
            for(int i=0;i<badges.size();++i){
                const auto& badge=badges[i];if(x>=badgeBox.right())break;
                const auto text=movementDecoratedText(badge.text,badge.direction);
                const int remaining=badges.size()-i;
                const qreal budget=qMax(1.0,(badgeBox.right()-x-separatorWidth*(remaining-1))/remaining);
                painter->setPen(badge.fixedRisk?themeColors().danger:MovementColorPolicy::colorFor(option.widget,badge.direction));
                x+=drawLedgerText(*painter,QRectF(x,badgeBox.top(),budget,badgeBox.height()),text,FontRole::Secondary14);
                if(i+1<badges.size()){
                    painter->setPen(themeColors().text3);
                    drawLedgerText(*painter,QRectF(x,badgeBox.top(),separatorWidth,badgeBox.height()),separator,FontRole::Secondary14);
                    x+=separatorWidth;
                }
            }
        };
        const auto instrument=index.siblingAtColumn(0).data(ChartInstrumentLabelRole).toStringList();
        const auto textWidth=[](const QString& text,FontRole role,bool numbers){
            QTextLayout layout(text,KeefiFonts::font(role));if(numbers)layout.setFormats(KeefiFonts::numberRuns(text));
            layout.beginLayout();auto line=layout.createLine();if(line.isValid())line.setLineWidth(1000000);layout.endLayout();return line.isValid()?line.naturalTextWidth():0;
        };
        const auto nameRect=view?view->visualRect(index.siblingAtColumn(0)):QRect{};
        const bool compactInstrument=view&&instrument.size()==3&&rowRect.width()>nameRect.width()&&
            textWidth(instrument[0],FontRole::BodyBold15,false)+textWidth(instrument[1],FontRole::Number11,true)+16>nameRect.width()-24;
        if(compactInstrument){
            // In a narrow workspace the name column can be smaller than a whole
            // ticker. Paint a shared three-line row through both cells' clips:
            // name; code/price; status/change. Each sibling repaints identical
            // shared text after its background, so no column erases the label.
            const QRectF title(rowRect.left()+12,rowRect.top()+2,rowRect.width()-24,20);
            const QRectF quote(title.left(),title.top()+20,title.width(),20);
            const QRectF change(title.left(),title.top()+40,title.width(),20);
            const auto priceLines=index.siblingAtColumn(1).data(Qt::DisplayRole).toString().split('\n');
            const auto direction=movementForSign(index.siblingAtColumn(1).data(ChangeDirectionRole).toInt());
            const auto changeText=movementDecoratedText(priceLines.value(1),direction);
            const auto priceWidth=textWidth(priceLines.value(0),FontRole::Secondary14,true);
            const auto changeWidth=textWidth(changeText,FontRole::Secondary14,true);
            const auto foreground=index.siblingAtColumn(0).data(Qt::ForegroundRole);
            const QColor nameInk=foreground.canConvert<QBrush>()?foreground.value<QBrush>().color():item.palette.color(QPalette::Text);
            painter->setPen(nameInk);
            const bool named=!instrument[0].isEmpty();
            drawLedgerText(*painter,title,named?instrument[0]:instrument[1],named?FontRole::BodyBold15:FontRole::Number11,Qt::AlignLeft,nullptr,!named);
            if(named){
                painter->setPen(themeColors().text2);
                drawLedgerText(*painter,QRectF(quote.left(),quote.top(),qMax(0.0,quote.width()-priceWidth-8),quote.height()),instrument[1],FontRole::Number11);
            }
            painter->setPen(item.palette.color(QPalette::Text));
            drawLedgerText(*painter,quote,priceLines.value(0),FontRole::Secondary14,Qt::AlignRight);
            painter->setPen(MovementColorPolicy::colorFor(option.widget,direction));
            drawLedgerText(*painter,change,changeText,FontRole::Secondary14,Qt::AlignRight);
            badgeBox=QRectF(change.left(),change.top(),qMax(0.0,change.width()-changeWidth-(changeText.isEmpty()?0:8)),change.height());
            drawBadges();painter->restore();return;
        }
        painter->setPen(item.palette.color(QPalette::Text));
        if(index.column()!=0){
            drawLedgerText(*painter,first,lines.value(0),FontRole::Secondary14,Qt::AlignRight);
            const auto direction=movementForSign(index.data(ChangeDirectionRole).toInt());
            painter->setPen(MovementColorPolicy::colorFor(option.widget,direction));
            drawLedgerText(*painter,second,movementDecoratedText(lines.value(1),direction),FontRole::Secondary14,Qt::AlignRight);
            if(badgeFooter)drawBadges();
            painter->restore();return;
        }
        if(instrument.size()==3){
            const auto name=instrument[0],code=instrument[1],market=instrument[2];
            const qreal codeWidth=qMin(textWidth(code,FontRole::Number11,true),first.width());
            const qreal marketWidth=qMin(textWidth(market,FontRole::Caption12,true),qMax(0.0,first.width()-codeWidth-8));
            const qreal nameWidth=qMin(textWidth(name,FontRole::BodyBold15,false),qMax(0.0,first.width()-codeWidth-marketWidth-16));
            qreal x=first.left();
            if(nameWidth>0){drawLedgerText(*painter,QRectF(x,first.top(),nameWidth,first.height()),name,FontRole::BodyBold15,Qt::AlignLeft,nullptr,false);x+=nameWidth+8;}
            drawLedgerText(*painter,QRectF(x,first.top(),codeWidth,first.height()),code,FontRole::Number11);x+=codeWidth+8;
            painter->setPen(themeColors().text3);
            drawLedgerText(*painter,QRectF(x,first.top(),qMax(0.0,first.right()-x),first.height()),market,FontRole::Caption12);
        }else{
        const auto titleFont=KeefiFonts::font(FontRole::BodyBold15);
        // Keep fractional glyph advance: rounding down by a subpixel can make
        // an otherwise fitting last Hangul syllable turn into an ellipsis.
        const qreal titleWidth=qMin(QFontMetricsF(titleFont).horizontalAdvance(lines.value(0)),qMax(0.0,box.width()*0.6));
        drawLedgerText(*painter,QRectF(first.left(),first.top(),titleWidth,first.height()),lines.value(0),FontRole::BodyBold15);
        painter->setPen(themeColors().text3);
        drawLedgerText(*painter,QRectF(first.left()+titleWidth+8,first.top(),qMax(0.0,box.width()-titleWidth-8),first.height()),lines.value(1),FontRole::Caption12);
        }
        drawBadges();
        painter->restore();
    }
};
} // namespace keefi
