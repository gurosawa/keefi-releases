#include "keefi/PlainToolTip.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MarketMap.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/KeefiVisuals.h"

#include <QHash>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace keefi {
namespace {
constexpr qreal HeaderHeight = 26, LegendHeight = 24, SectionGap = 10, TileGap = 3, GroupLabel = 16, GroupGap = 6;

double changeValue(const QString& percent, bool* ok) {
    bool valid = false;
    const double value = percent.toDouble(&valid);
    valid=valid&&!percent.isEmpty()&&std::isfinite(value);
    if (ok) *ok = valid;
    return valid ? value : 0;
}
QColor tileColor(const QObject* context,const QString& percent) {
    bool ok = false;
    const double change = changeValue(percent, &ok);
    const double size = std::min(std::abs(change), 3.0);
    if (!ok || size < 0.1) return themeColors().surface;
    // Rise green, fall red (user choice), three steps up to ±3%.
    const int step = size < 1 ? 0 : size < 2 ? 1 : 2;
    return MovementColorPolicy::heatFor(context,change>0?MovementDirection::Up:MovementDirection::Down,step);
}
QString changeText(const QString& percent) {
    bool ok = false;
    const double change = changeValue(percent, &ok);
    if (!ok) return QObject::tr("변동 미확인");
    const auto text = QString::number(change, 'f', 2);
    const auto shown=text.toDouble();
    return shown > 0 ? QStringLiteral("▲ +") + text + "%" : shown < 0 ? QStringLiteral("▼ ") + text + "%" : QStringLiteral("0.00%");
}

qreal captionHeight(const QString& text, const QFont& font, qreal width) {
    QTextLayout layout(text, font);QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout.setTextOption(option);
    qreal height=0;layout.beginLayout();
    while(true){auto line=layout.createLine();if(!line.isValid())break;line.setLineWidth(std::max(1.0,width));height+=line.height();}
    layout.endLayout();return height;
}
struct Caption {
    QString text;
    FontRole role;
    QColor ink;
    QRectF rect;
};
void paintCaption(QPainter& painter,const Caption& caption,QPointF origin) {
    const auto font=KeefiFonts::font(caption.role);QTextLayout layout(caption.text,font);
    QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout.setTextOption(option);
    qreal height=0;layout.beginLayout();
    while(true){auto line=layout.createLine();if(!line.isValid())break;line.setLineWidth(caption.rect.width());line.setPosition({0,height});height+=line.height();}
    layout.endLayout();painter.setPen(caption.ink);
    layout.draw(&painter,origin+caption.rect.topLeft()+QPointF(0,(caption.rect.height()-height)/2));
}
// Flow complete phrases instead of shortening them. The measured height is
// reserved before placing tiles, so an extra caption row never covers data.
struct CaptionFlow {
    qreal width,baseHeight,rowGap,x=0,y=0,rowHeight=0;
    QRectF place(qreal wantedWidth,qreal wantedHeight,qreal gap,bool right=false) {
        const qreal itemWidth=std::min(width,wantedWidth);
        qreal before=x>0?gap:0;
        if(x>0&&x+before+itemWidth>width){y+=rowHeight+rowGap;x=0;rowHeight=0;before=0;}
        const qreal left=right?std::max(x+before,width-itemWidth):x+before;
        const qreal height=std::max(baseHeight,wantedHeight);rowHeight=std::max(rowHeight,height);x=left+itemWidth;
        return {left,y,itemWidth,height};
    }
    qreal height() const {return y+rowHeight;}
};
struct CaptionPlan {QList<Caption> captions;qreal height=0;};
CaptionPlan headerPlan(const QObject* context,const MarketMapSection& section,const QHash<QString,QList<int>>&,
                       qreal width,bool drill,bool hovered=false) {
    CaptionPlan plan;CaptionFlow flow{std::max(1.0,width),HeaderHeight-6,2};
    const auto append=[&](const QString& text,FontRole role,QColor ink,bool right=false){
        if(text.isEmpty())return;const auto font=KeefiFonts::font(role);
        const qreal itemWidth=std::min(flow.width,QFontMetricsF(font).horizontalAdvance(text));
        plan.captions.append({text,role,ink,flow.place(itemWidth,captionHeight(text,font,itemWidth),8,right)});
    };
    append(drill?QObject::tr("‹ 전체 · ")+section.title:section.title+(section.available?QObject::tr(" ›"):QString{}),
           FontRole::SecondaryStrong14,hovered?themeColors().accentText:themeColors().text);
    if(section.available&&!section.tiles.isEmpty()) {
        double weighted=0,weights=0,known=0;
        for(const auto& tile:section.tiles)known+=std::max(0.0,tile.weight);
        for(const auto& tile:section.tiles){bool ok=false;const double value=changeValue(tile.changePercent,&ok);
            const double weight=known>0?std::max(0.0,tile.weight):1.0;
            if(ok){weighted+=value*weight;weights+=weight;}}
        if(weights>0){
            const auto shown=QString::number(weighted/weights,'f',2).toDouble();
            append(changeText(QString::number(weighted/weights,'f',4)),FontRole::Number11,
                   MovementColorPolicy::colorFor(context,movementForSign(shown>0?1:shown<0?-1:0)));

        }
    }
    if(!section.status.isEmpty())append(QStringLiteral("· ")+section.status,FontRole::Caption12,themeColors().text3);
    plan.height=flow.height()+6;return plan;
}
struct LegendPlan {QList<Caption> captions;QList<QPair<QColor,QRectF>> swatches;qreal height=0;};
LegendPlan legendPlan(const QObject* context,qreal width) {
    LegendPlan plan;CaptionFlow flow{std::max(1.0,width),LegendHeight,0};
    const auto append=[&](const QString& text,QColor ink,qreal gap){
        const auto font=KeefiFonts::font(FontRole::Caption12);const qreal itemWidth=std::min(flow.width,QFontMetricsF(font).horizontalAdvance(text));
        plan.captions.append({text,FontRole::Caption12,ink,flow.place(itemWidth,captionHeight(text,font,itemWidth),gap)});
    };
    append(QObject::tr("크기 · 시가총액"),themeColors().text3,0);
    append(QObject::tr("색 · 전일 대비(코인 24시간)"),themeColors().text3,18);
    append(QStringLiteral("▼ -3%"),MovementColorPolicy::colorFor(context,MovementDirection::Down),8);
    const QList<QColor> colors{MovementColorPolicy::heatFor(context,MovementDirection::Down,2),MovementColorPolicy::heatFor(context,MovementDirection::Down,1),
        MovementColorPolicy::heatFor(context,MovementDirection::Down,0),themeColors().surface,MovementColorPolicy::heatFor(context,MovementDirection::Up,0),
        MovementColorPolicy::heatFor(context,MovementDirection::Up,1),MovementColorPolicy::heatFor(context,MovementDirection::Up,2)};
    const auto group=flow.place(colors.size()*15-3,LegendHeight,6);
    for(int i=0;i<colors.size();++i)plan.swatches.append({colors[i],QRectF(group.left()+i*15,group.center().y()-5,12,10)});
    append(QStringLiteral("+3% ▲"),MovementColorPolicy::colorFor(context,MovementDirection::Up),3);
    plan.height=flow.height();return plan;
}
} // namespace

QList<QRectF> squarifiedRects(const QList<double>& areas, QRectF box) {
    QList<QRectF> out;
    int i = 0;
    while (i < areas.size()) {
        const double side = std::max(1e-9, std::min(box.width(), box.height()));
        const auto worst = [side](const QList<double>& row) {
            const double sum = std::accumulate(row.cbegin(), row.cend(), 0.0);
            const double largest = *std::max_element(row.cbegin(), row.cend()), smallest = *std::min_element(row.cbegin(), row.cend());
            if (sum <= 0 || smallest <= 0) return 1e18;
            return std::max(side * side * largest / (sum * sum), (sum * sum) / (side * side * smallest));
        };
        QList<double> row{areas[i]};
        int j = i + 1;
        while (j < areas.size()) {
            auto next = row; next.append(areas[j]);
            if (worst(next) > worst(row)) break;
            row = next; ++j;
        }
        const double sum = std::accumulate(row.cbegin(), row.cend(), 0.0);
        if (box.width() >= box.height()) {
            const double width = box.height() > 0 ? sum / box.height() : 0;
            double y = box.top();
            for (const double area : row) { const double h = width > 0 ? area / width : 0; out.append(QRectF(box.left(), y, width, h)); y += h; }
            box.setLeft(box.left() + width);
        } else {
            const double height = box.width() > 0 ? sum / box.width() : 0;
            double x = box.left();
            for (const double area : row) { const double w = height > 0 ? area / height : 0; out.append(QRectF(x, box.top(), w, height)); x += w; }
            box.setTop(box.top() + height);
        }
        i = j;
    }
    return out;
}

MarketMapWidget::MarketMapWidget(QWidget* parent) : QWidget(parent) {
    setObjectName("marketMap");
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("시장 지도"));
    setAccessibleDescription(tr("크기는 표시 종목의 시가총액, 색은 등락입니다. 화살표로 종목을 옮기고 Enter로 오른쪽 요약을 확인합니다."));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    bindMovementPresentation(this,[this]{update();});
    bindThemePresentation(this,[this]{update();});
}

void MarketMapWidget::setSections(const QList<MarketMapSection>& sections) {
    if (sections_ == sections) return;
    sections_ = sections;
    relayout();refreshAccessibility();
    update();
}

void MarketMapWidget::setFilter(const QString& key) {
    const auto filter = key.isEmpty() ? QStringLiteral("all") : key;
    if (filter_ == filter) return;
    filter_ = filter;
    relayout();refreshAccessibility();
    update();
}

void MarketMapWidget::select(const QString& section, const QString& id) {
    selectedSection_ = section;
    selectedId_ = id;
    update();
}

QList<QPair<QString, QRectF>> MarketMapWidget::tileRects() const {
    QList<QPair<QString, QRectF>> out;
    for (const auto& p : placed_) out.append({p.section + ":" + p.id, p.rect});
    return out;
}

QRectF MarketMapWidget::actionRect(const QString& section) const {
    for (const auto& box : boxes_) if (sections_.value(box.section).key == section) return box.action;
    return {};
}

void MarketMapWidget::setEvidenceStates(const QHash<QString, QString>& states) {if (evidenceStates_ == states) return; evidenceStates_=states;refreshAccessibility();update();}

void MarketMapWidget::setEvidenceNumbers(const QHash<QString, QList<int>>& numbers) {
    if (evidenceNumbers_ == numbers) return;
    evidenceNumbers_ = numbers;
    refreshAccessibility();update();
}
void MarketMapWidget::refreshAccessibility() {
    QStringList descriptions{tr("크기는 표시 종목의 시가총액, 색은 등락입니다. 화살표로 종목을 옮기고 Enter로 오른쪽 요약을 확인합니다.")};
    for(const auto& placed:placed_)descriptions.append(usabilityPublicText(placed.id)+" "+usabilityFreshness(evidenceStates_.value(placed.section+":"+placed.id)));
    setAccessibleDescription(descriptions.join('\n'));
}

void MarketMapWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout();
}

void MarketMapWidget::relayout() {
    placed_.clear();
    boxes_.clear();
    groups_.clear();
    QList<int> shown;
    for (int i = 0; i < sections_.size(); ++i)
        if (filter_ == "all" || sections_[i].key == filter_) shown.append(i);
    const QRectF area = QRectF(rect()).adjusted(0, 0, 0, -legendPlan(this,width()).height - 6);
    if (shown.isEmpty() || area.width() < 40 || area.height() < 60) return;
    QList<QRectF> frames;
    if (shown.size() == 1) frames.append(area);
    else if (shown.size() == 3 && area.width() >= 700) {
        // Mockup: coins on the left at full height, the two stock markets stacked on the right.
        const double left = (area.width() - SectionGap) * 0.54, half = (area.height() - SectionGap) / 2;
        frames.append(QRectF(area.left(), area.top(), left, area.height()));
        frames.append(QRectF(area.left() + left + SectionGap, area.top(), area.width() - left - SectionGap, half));
        frames.append(QRectF(area.left() + left + SectionGap, area.top() + half + SectionGap, area.width() - left - SectionGap, area.height() - half - SectionGap));
    } else {
        // The first section (코인) takes the top half; the rest share the bottom row.
        const double topHeight = (area.height() - SectionGap) * 0.52;
        frames.append(QRectF(area.left(), area.top(), area.width(), topHeight));
        const int rest = int(shown.size()) - 1;
        const double width = (area.width() - SectionGap * (rest - 1)) / rest;
        for (int i = 0; i < rest; ++i)
            frames.append(QRectF(area.left() + i * (width + SectionGap), area.top() + topHeight + SectionGap, width, area.height() - topHeight - SectionGap));
    }
    for (int n = 0; n < shown.size(); ++n) {
        const auto& section = sections_[shown[n]];
        SectionBox box;
        box.section = shown[n];
        box.box = frames[n];
        const qreal headerHeight=headerPlan(this,section,evidenceNumbers_,frames[n].width(),shown.size()==1).height;
        box.map = frames[n].adjusted(0, headerHeight, 0, 0);
        box.header = QRectF(frames[n].left(), frames[n].top(), frames[n].width(), headerHeight - 4);
        if (!section.available || section.tiles.isEmpty()) {
            const QSizeF button(fontMetrics().horizontalAdvance(section.actionText) + 36, 34);
            box.action = section.actionText.isEmpty() ? QRectF()
                : QRectF(box.map.center().x() - button.width() / 2, box.map.center().y() + 30, button.width(), button.height());
            boxes_.append(box);
            continue;
        }
        // Largest first; the overview shows only the leaders of each section.
        QList<int> order(section.tiles.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return section.tiles[a].weight > section.tiles[b].weight; });
        const bool drill = shown.size() == 1;
        if (!drill && order.size() > section.overviewLimit) order = order.mid(0, section.overviewLimit);
        const auto placeTiles = [&](const QList<int>& indexes, const QRectF& area) {
            double total = 0;
            for (const int index : indexes) total += std::max(0.0, section.tiles[index].weight);
            QList<double> areas;
            for (const int index : indexes) {
                const double share = total > 0 ? std::max(0.0, section.tiles[index].weight) / total : 1.0 / indexes.size();
                areas.append(std::max(share * area.width() * area.height(), 1.0));
            }
            const auto rects = squarifiedRects(areas, area);
            for (int k = 0; k < rects.size(); ++k)
                placed_.append({section.key, section.tiles[indexes[k]].id, rects[k].adjusted(0, 0, -TileGap, -TileGap), indexes[k]});
        };
        bool grouped = false;
        if (drill) for (const int index : order) grouped |= !section.tiles[index].group.isEmpty();
        if (!grouped) placeTiles(order, box.map);
        else {
            // Drill-down: sectors sized by their total, tiles inside each sector.
            QList<QString> names; QHash<QString, QList<int>> members; QHash<QString, double> totals;
            for (const int index : order) {
                const auto group = section.tiles[index].group.isEmpty() ? tr("기타") : section.tiles[index].group;
                if (!members.contains(group)) names.append(group);
                members[group].append(index);
                totals[group] += std::max(0.0, section.tiles[index].weight);
            }
            std::stable_sort(names.begin(), names.end(), [&](const QString& a, const QString& b) { return totals.value(a) > totals.value(b); });
            double all = 0; for (const auto& name : names) all += totals.value(name);
            QList<double> areas;
            for (const auto& name : names) areas.append(std::max((all > 0 ? totals.value(name) / all : 1.0 / names.size()) * box.map.width() * box.map.height(), 1.0));
            const auto sectorRects = squarifiedRects(areas, box.map);
            for (int g = 0; g < names.size(); ++g) {
                const QRectF sector = sectorRects[g].adjusted(0, 0, -GroupGap, -GroupGap);
                const bool labelled = sector.height() > GroupLabel * 2.5 && sector.width() > 40;
                groups_.append({names[g], sector});
                placeTiles(members.value(names[g]), labelled ? sector.adjusted(0, GroupLabel, 0, 0) : sector);
            }
        }
        boxes_.append(box);
    }
}

void MarketMapWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    for (const auto& box : boxes_) {
        const auto& section = sections_[box.section];
        painter.save();painter.setClipRect(box.header,Qt::IntersectClip);
        const auto header=headerPlan(this,section,evidenceNumbers_,box.header.width(),boxes_.size()==1,hoverKey_=="header:"+section.key);
        for(const auto& caption:header.captions)paintCaption(painter,caption,box.header.topLeft());
        painter.restore();
        if (!section.available || section.tiles.isEmpty()) {
            // Reason and one action; never placeholder prices.
            painter.setPen(QPen(themeColors().rule, 1, Qt::DashLine));
            painter.setBrush(themeColors().bg);
            painter.drawRect(box.map.adjusted(0.5, 0.5, -0.5, -0.5));
            const QFont heading = KeefiFonts::font(FontRole::SecondaryStrong14);
            painter.setFont(heading); painter.setPen(themeColors().text);
            const QRectF text = box.map.adjusted(20, 0, -20, 0);
            painter.drawText(QRectF(text.left(), box.map.center().y() - 52, text.width(), 22), Qt::AlignCenter, section.lockedTitle);
            const QFont body = KeefiFonts::font(FontRole::Caption12);
            painter.setFont(body); painter.setPen(themeColors().text3);
            painter.drawText(QRectF(text.left(), box.map.center().y() - 26, text.width(), 52), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, section.lockedText);
            if (!box.action.isNull()) {
                const bool hover = hoverKey_ == "action:" + section.key;
                painter.setPen(Qt::NoPen); painter.setBrush(hover ? themeColors().text2 : themeColors().text);
                painter.drawRoundedRect(box.action, 10, 10);
                const QFont action = KeefiFonts::font(FontRole::SecondaryStrong14);
                painter.setFont(action); painter.setPen(themeColors().bg);
                painter.drawText(box.action, Qt::AlignCenter, section.actionText);
            }
        }
    }
    for (const auto& p : placed_) {
        const auto section = std::find_if(sections_.cbegin(), sections_.cend(), [&](const auto& s) { return s.key == p.section; });
        if (section == sections_.cend()) continue;
        const auto& tile = section->tiles[p.index];
        const bool selected = p.section == selectedSection_ && p.id == selectedId_;
        const bool hover = hoverKey_ == p.section + ":" + p.id;
        QColor fill = tileColor(this,tile.changePercent);
        if (hover) fill = fill.lighter(122);
        painter.setPen(Qt::NoPen); painter.setBrush(fill);
        painter.drawRect(p.rect);
        if (selected) {
            painter.setPen(QPen(themeColors().accentText, 2)); painter.setBrush(Qt::NoBrush);
            painter.drawRect(p.rect.adjusted(1, 1, -1, -1));
        }
        const QRectF inner = p.rect.adjusted(8, 6, -8, -6);
        if (inner.width() < 24 || inner.height() < 12) continue;
        const QFont name = KeefiFonts::font(inner.width() > 100 && inner.height() > 54 ? FontRole::Title17 : FontRole::CaptionStrong12);
        const QFont detail = KeefiFonts::font(FontRole::Number11);
        const auto change = changeText(tile.changePercent);
        const bool showChange = inner.width() > 46 && inner.height() > 30 &&
                                QFontMetricsF(detail).horizontalAdvance(change) <= inner.width();
        const bool showPrice = inner.width() > 96 && inner.height() > 66;
        double y = inner.bottom();
        painter.setFont(detail);
        if (showPrice) {
            const auto state=usabilityFreshness(evidenceStates_.value(p.section+":"+p.id));
            const auto stateFont=KeefiFonts::font(FontRole::Caption12);
            const qreal reserve=state.isEmpty()?0:QFontMetricsF(stateFont).horizontalAdvance(state)+8;
            painter.setPen(themeColors().text2);
            painter.drawText(QRectF(inner.left(),y-16,qMax(1.0,inner.width()-reserve),16),Qt::AlignLeft|Qt::AlignVCenter,
                             QFontMetricsF(detail).elidedText(usabilityPublicText(tile.price),Qt::ElideRight,qMax(1.0,inner.width()-reserve)));
            if(!state.isEmpty()){painter.setFont(stateFont);painter.drawText(QRectF(inner.right()-reserve+8,y-16,reserve-8,16),Qt::AlignRight|Qt::AlignVCenter,state);painter.setFont(detail);}
            y -= 17;
        }
        if (showChange) {
            painter.setPen(themeColors().text);
            painter.drawText(QRectF(inner.left(), y - 16, inner.width(), 16), Qt::AlignLeft | Qt::AlignVCenter, change);
            y -= 17;
        }
        painter.setFont(name); painter.setPen(themeColors().text);
        const double nameHeight = QFontMetricsF(name).height();
        painter.drawText(QRectF(inner.left(), y - nameHeight, inner.width(), nameHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetricsF(name).elidedText(tile.label, Qt::ElideRight, inner.width()));
    }
    for (const auto& group : groups_) {
        if (group.box.height() <= GroupLabel * 2.5 || group.box.width() <= 40) continue;
        const QFont label = KeefiFonts::font(FontRole::CaptionStrong12);
        painter.setFont(label); painter.setPen(themeColors().text3);
        painter.drawText(QRectF(group.box.left() + 2, group.box.top(), group.box.width() - 4, GroupLabel - 2), Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetricsF(label).elidedText(group.label, Qt::ElideRight, group.box.width() - 4));
    }
    const auto legend=legendPlan(this,width());const QPointF origin(0,height()-legend.height);
    for(const auto& caption:legend.captions)paintCaption(painter,caption,origin);
    for(const auto& swatch:legend.swatches){painter.setPen(Qt::NoPen);painter.setBrush(swatch.first);painter.drawRect(swatch.second.translated(origin));}
}

void MarketMapWidget::mousePressEvent(QMouseEvent* event) {
    const auto at = event->position();
    for (const auto& box : boxes_)
        if (!box.action.isNull() && box.action.contains(at)) { emit actionRequested(sections_[box.section].key); return; }
    for (const auto& box : boxes_)
        if (box.header.contains(at)) { emit sectionRequested(boxes_.size() == 1 ? QStringLiteral("all") : sections_[box.section].key); return; }
    for (const auto& p : placed_)
        if (p.rect.contains(at)) {
            select(p.section, p.id);
            emit tileSelected(p.section, p.id);
            emit tileActivated(p.section, p.id);
            return;
        }
}

void MarketMapWidget::mouseMoveEvent(QMouseEvent* event) {
    QString key;
    const auto at = event->position();
    for (const auto& box : boxes_) if (!box.action.isNull() && box.action.contains(at)) key = "action:" + sections_[box.section].key;
    for (const auto& box : boxes_) if (key.isEmpty() && box.header.contains(at)) key = "header:" + sections_[box.section].key;
    for (const auto& p : placed_) if (key.isEmpty() && p.rect.contains(at)) key = p.section + ":" + p.id;
    if (key != hoverKey_) { hoverKey_ = key; setCursor(key.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor); update(); }
}

void MarketMapWidget::leaveEvent(QEvent* event) {
    hoverKey_.clear();
    update();
    QWidget::leaveEvent(event);
}

void MarketMapWidget::keyPressEvent(QKeyEvent* event) {
    if (placed_.isEmpty()) { QWidget::keyPressEvent(event); return; }
    int current = -1;
    for (int i = 0; i < placed_.size(); ++i) if (placed_[i].section == selectedSection_ && placed_[i].id == selectedId_) current = i;
    int next = current;
    switch (event->key()) {
    case Qt::Key_Right: case Qt::Key_Down: next = (current + 1) % placed_.size(); break;
    case Qt::Key_Left: case Qt::Key_Up: next = current <= 0 ? int(placed_.size()) - 1 : current - 1; break;
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space:
        if (current >= 0) {emit tileSelected(selectedSection_, selectedId_);emit tileActivated(selectedSection_, selectedId_);}
        return;
    default: QWidget::keyPressEvent(event); return;
    }
    select(placed_[next].section, placed_[next].id);
    emit tileSelected(selectedSection_, selectedId_);
}

bool MarketMapWidget::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        for (const auto& p : placed_)
            if (p.rect.contains(help->pos())) {
                const auto section = std::find_if(sections_.cbegin(), sections_.cend(), [&](const auto& s) { return s.key == p.section; });
                const auto& tile = section->tiles[p.index];
                QToolTip::showText(help->globalPos(), plainToolTip(usabilityPublicText(tile.name + "\n" + tile.price + " · " + changeText(tile.changePercent)+"\n"+section->source)), this);
                return true;
            }
        QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

} // namespace keefi
