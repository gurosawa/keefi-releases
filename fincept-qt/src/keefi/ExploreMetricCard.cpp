#include "keefi/ExploreMetricCard.h"
#include "keefi/InfoButton.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QHash>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSet>
#include <QShowEvent>
#include <QTimeZone>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>
namespace keefi {
namespace {
QList<ExploreHistoryPoint> usableHistory(const ExploreMetricCardData& data) {
    const auto& history=data.history;
    if(!history.available||data.sourceKey.isEmpty()||data.unit.isEmpty()||history.sourceKey!=data.sourceKey||history.unit!=data.unit
        ||history.points.size()<2||history.points.size()>ExploreMetricCard::MaxHistoryPoints)return {};
    qint64 previous=0;for(const auto& point:history.points){if(point.atMs<=previous||!std::isfinite(point.value))return {};previous=point.atMs;}
    return history.points;
}
bool hasConnectedHistory(const QList<ExploreHistoryPoint>& points) {
    for(int i=1;i<points.size();++i)if(points[i].connectsPrevious)return true;return false;
}
QString knownProvider(const QString& value) {
    static const QSet<QString> known={"업비트","빗썸","바이낸스","Binance","OKX","토스증권","키움증권","LS증권","한국투자증권","Coinbase","Coinbase Exchange","ECB","Nasdaq","네이버 증권","CoinGecko","미국 재무부","Solana","원천 미제공"};
    return known.contains(value)?value:QStringLiteral("원천 미제공");
}
// Status can vary by context for the same observation. Keep every identity
// field independent, including arbitrary private IDs and original clock values.
struct EvidenceIdentity {
    QString id, provider, sourceDate;
    qint64 sourceTimeMs, receivedAtMs;
    bool complete;
    bool operator==(const EvidenceIdentity&) const = default;
    friend size_t qHash(const EvidenceIdentity& key, size_t seed) noexcept {
        return qHashMulti(seed, key.id, key.provider, key.sourceDate,
                          key.sourceTimeMs, key.receivedAtMs, key.complete);
    }
};
QString knownState(const ExploreMetricEvidence& e) {
    static const QSet<QString> states={"유효","지연","미확인","불완전","일별 공시"};
    return states.contains(e.state)?e.state:!e.state.isEmpty()?QStringLiteral("미확인"):e.complete?QStringLiteral("유효"):QStringLiteral("불완전");
}
QString span(const QString& text,FontRole role,const QColor& color) {
    const auto font=KeefiFonts::font(role);
    return QString("<span style=\"font-family:'%1';font-size:%2px;color:%3;\">%4</span>")
        .arg(font.family().toHtmlEscaped()).arg(font.pixelSize()).arg(color.name(),text.toHtmlEscaped());
}
QString captionText(const QString& original) {
    // Keep the display string intact while giving digits/codes their real face.
    static const QRegularExpression numeric(QStringLiteral("[+\\-]?[0-9][0-9,.:/%+\\-]*(?![A-Za-z0-9_])|(?<![A-Za-z0-9_])[A-Z0-9][A-Z0-9_./\\-]*(?![A-Za-z0-9_])"));
    QString rich;int offset=0;auto matches=numeric.globalMatch(original);
    while(matches.hasNext()){const auto match=matches.next();rich+=span(original.mid(offset,match.capturedStart()-offset),FontRole::Caption12,themeColors().text3);
        rich+=span(match.captured(),FontRole::Number11,themeColors().text3);offset=match.capturedEnd();}
    rich+=span(original.mid(offset),FontRole::Caption12,themeColors().text3);return rich.replace("\n","<br>");
}
QString compactFreshness(const ExploreMetricCardData& data) {
    QStringList states{data.freshnessText};for(const auto& evidence:data.evidence)states.append(evidence.state);
    return usabilityFreshness(states.join(' '));
}
QString compactSources(const ExploreMetricCardData& data) {
    QStringList sources;for(const auto& evidence:data.evidence){
        const auto source=usabilitySourceTime(knownProvider(evidence.provider),evidence.sourceTimeMs);
        if(!source.isEmpty()&&!sources.contains(source))sources.append(source);
    }
    return sources.join(QStringLiteral(" / "));
}
QString displayedInformation(const ExploreMetricCardData& data) {
    QStringList lines;
    for(const auto& original:data.publicInformation.split('\n')){
        const auto line=usabilityPublicText(original);
        if(line.isEmpty()||line.startsWith(QStringLiteral("원천:"))||line.startsWith(QStringLiteral("원천 "))||line.startsWith(QStringLiteral("수신"))
            ||line.startsWith(QStringLiteral("관측 "))||line.startsWith(QStringLiteral("관측일:"))||line.startsWith(QStringLiteral("시세 반영 시각"))
            ||line.startsWith(QStringLiteral("실제 기록 배열 미제공")))continue;
        lines.append(line);
    }
    const auto sources=compactSources(data);if(!sources.isEmpty())lines.append(sources);
    return lines.join('\n');
}
QLabel* label(QWidget* parent,const char* name,FontRole role) {
    auto* result=new QLabel(parent);result->setObjectName(name);result->setTextFormat(Qt::PlainText);result->setFont(KeefiFonts::font(role));result->setMinimumWidth(0);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse);return result;
}
class MetricHistoryWidget final : public QWidget {
  public:
    explicit MetricHistoryWidget(QWidget* parent) : QWidget(parent) {setObjectName("metricSparkline");setFixedSize(110,24);setAccessibleName(tr("실제 관측 기록"));bindThemePresentation(this,[this]{update();});}
    void setWide(bool wide,int height=96) {
        wide_=wide;
        if(wide){setMinimumSize(240,height);setMaximumSize(460,height);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);}
        else setFixedSize(110,24);
        update();
    }
    void setHistory(const QList<ExploreHistoryPoint>& points) {
        points_=points;setAccessibleDescription(points.isEmpty()?QString{}:tr("실제 UTC 시각의 관측 %1개 · 확인되지 않은 연결은 끊어 표시").arg(points.size()));update();
    }
  protected:
    void paintEvent(QPaintEvent*) override {
        if(points_.size()<2)return;const auto captionHeight=wide_?18.0:0.0;
        const auto box=QRectF(rect()).adjusted(1,wide_?6:2,wide_?-30:-1,-2-captionHeight);if(box.width()<=0||box.height()<=0)return;
        long double low=points_.first().value,high=low;for(const auto& point:points_){low=std::min(low,static_cast<long double>(point.value));high=std::max(high,static_cast<long double>(point.value));}
        // A wide percent line keeps zero in view so a sign change is visible at a glance.
        if(wide_&&zero_){low=std::min(low,0.0L);high=std::max(high,0.0L);}
        if(wide_&&high-low<=0){low-=1;high+=1;}
        const auto first=points_.first().atMs,last=points_.last().atMs;const long double duration=static_cast<long double>(last-first);if(duration<=0)return;
        const long double range=high-low;if(!std::isfinite(range))return;
        const auto coordinate=[&](const ExploreHistoryPoint& point){const long double x=static_cast<long double>(point.atMs-first)/duration;
            const long double y=range>0?(static_cast<long double>(point.value)-low)/range:0.5L;return QPointF(box.left()+double(x)*box.width(),box.bottom()-double(y)*box.height());};
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        if(wide_){
            if(zero_){
                const auto zero=range>0?box.bottom()-double((0.0L-low)/range)*box.height():box.center().y();
                painter.setPen(QPen(themeColors().text3,1,Qt::DashLine));painter.drawLine(QPointF(box.left(),zero),QPointF(box.right(),zero));
                painter.setFont(KeefiFonts::font(FontRole::Number11));painter.setPen(themeColors().text3);
                painter.drawText(QRectF(box.right()+4,zero-9,26,18),Qt::AlignLeft|Qt::AlignVCenter,QStringLiteral("0%"));
            }
            painter.setFont(KeefiFonts::font(FontRole::Caption12));painter.setPen(themeColors().text3);
            const QRectF captions(box.left(),height()-captionHeight,box.width(),captionHeight);
            painter.drawText(captions,Qt::AlignLeft|Qt::AlignVCenter,usabilityText(UsabilityText::FlowHistoryStart));
            painter.drawText(captions,Qt::AlignRight|Qt::AlignVCenter,usabilityText(UsabilityText::FlowHistoryEnd));
        }
        painter.setPen(QPen(wide_?themeColors().text:themeColors().text2,wide_?1.5:1.2));
        QPointF previous;for(int index=0;index<points_.size();++index){const auto current=coordinate(points_[index]);if(index>0&&points_[index].connectsPrevious)painter.drawLine(previous,current);previous=current;}
    }
  public:
    void setZeroBaseline(bool zero){zero_=zero;update();}
  private:
    QList<ExploreHistoryPoint> points_;
    bool wide_=false,zero_=false;
};
}
ExploreEvidenceFooter::ExploreEvidenceFooter(QWidget* parent):QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground,true);
    setProperty("exploreEvidenceFooter",true);auto* rows=new QVBoxLayout(this);rows->setContentsMargins(0,12,0,0);rows->setSpacing(4);rows->setAlignment(Qt::AlignTop);
    setThemeStyle(this,"QWidget[exploreEvidenceFooter=true]{border-top:1px solid ${rule};} QLabel{border:0;background:transparent;}");
    bindThemePresentation(this,[this]{refresh();});hide();
}
void ExploreEvidenceFooter::setEntries(const QList<ExploreMetricEvidence>& entries,int start) {
    start=qMax(1,start);if(entries_==entries&&start_==start)return;entries_=entries;start_=start;refresh();
}
int ExploreEvidenceFooter::evidenceCount()const{return count_;}
QList<int> ExploreEvidenceFooter::numbers()const{return numbers_;}
QList<ExploreMetricEvidence> ExploreEvidenceFooter::entries()const{return entries_;}
void ExploreEvidenceFooter::refresh() {
    while(auto* item=layout()->takeAt(0)){delete item->widget();delete item;}
    numbers_.clear();
    numbers_.reserve(entries_.size());
    struct NumberedState { int number; QString state; };
    QHash<EvidenceIdentity, NumberedState> observations;
    observations.reserve(entries_.size());
    // IDs remain in memory. Numbering follows first occurrence, independent of hash order.
    for (const auto& entry : std::as_const(entries_)) {
        const EvidenceIdentity key{entry.id, entry.provider, entry.sourceDate,
                                   entry.sourceTimeMs, entry.receivedAtMs, entry.complete};
        auto found = observations.find(key);
        if (found == observations.end()) {
            const int next = start_ + int(observations.size());
            found = observations.insert(key, {next, knownState(entry)});
        } else if (found->state != knownState(entry)) {
            found->state = QStringLiteral("미확인");
        }
        numbers_.append(found->number);
    }
    count_ = int(observations.size());
    // Keep the evidence book for its owning view without rendering a second
    // provenance surface. References no longer consume layout space.
    hide();updateGeometry();
}
struct ExploreMetricCard::Private {
    ExploreMetricCard* q=nullptr;ExploreMetricCardData data;QList<ExploreHistoryPoint> plotted;
    QGridLayout* row=nullptr;QWidget* names=nullptr;bool compact=false,arranged=false;
    ExploreMetricCard::Presentation presentation=ExploreMetricCard::Presentation::Row;
    QWidget *action=nullptr,*state=nullptr,*valueLine=nullptr,*rule=nullptr;
    QPointer<QWidget> headerDetail;
    QLabel *title=nullptr,*freshness=nullptr,*value=nullptr,*unit=nullptr,*change=nullptr,*reference=nullptr;InfoButton* info=nullptr;MetricHistoryWidget* history=nullptr;
    QPointer<MovementColorPolicy> policy;QMetaObject::Connection policyChanged,policyDestroyed;
    bool numeric=false;
    // Hero values use 44px when value, unit and state fit the line; otherwise 22px.
    void fitValue() {
        auto role=numeric?FontRole::Value22:FontRole::Secondary14;
        if(numeric&&presentation==ExploreMetricCard::Presentation::Hero){
            // Explicit visibility: a card hidden for a moment must not pick a different size.
            const auto reserve=(unit->isVisibleTo(q)?QFontMetrics(unit->font()).horizontalAdvance(unit->text())+8:0)+(state&&state->isVisibleTo(q)?state->sizeHint().width()+8:0)+(freshness->isVisibleTo(q)?freshness->sizeHint().width()+8:0);
            const auto available=q->width()>0?q->width()-reserve-4:INT_MAX;
            if(QFontMetrics(KeefiFonts::font(FontRole::Price44)).horizontalAdvance(value->text())<=available)role=FontRole::Price44;
        }
        if(value->font()!=KeefiFonts::font(role))value->setFont(KeefiFonts::font(role));
    }
    void arrangeStacked(bool narrow) {
        const bool hero=presentation==ExploreMetricCard::Presentation::Hero;
        const bool summary=presentation==ExploreMetricCard::Presentation::Summary;
        for(auto* widget:{names,static_cast<QWidget*>(info),static_cast<QWidget*>(value),static_cast<QWidget*>(unit),static_cast<QWidget*>(freshness),static_cast<QWidget*>(change),static_cast<QWidget*>(history),static_cast<QWidget*>(reference),action,valueLine})
            if(widget)row->removeWidget(widget);
        for(int column=0;column<8;++column)row->setColumnStretch(column,0);
        if(!valueLine){valueLine=new QWidget(q);valueLine->setObjectName("metricValueLine");auto* line=new QHBoxLayout(valueLine);line->setContentsMargins(0,0,0,0);line->setSpacing(8);}
        auto* line=static_cast<QHBoxLayout*>(valueLine->layout());while(auto* item=line->takeAt(0))delete item;
        value->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);line->addWidget(value,0,Qt::AlignBottom);line->addWidget(unit,0,Qt::AlignBottom);
        if(state)line->addWidget(state,0,Qt::AlignBottom);line->addWidget(freshness,0,Qt::AlignBottom);line->addStretch(1);
        // Stacked cards share one structure: title line, value line, then notes, all from the top.
        // Notes start at their top edge even when the dashboard gives them a fixed height.
        change->setWordWrap(true);change->setAlignment(Qt::AlignLeft|Qt::AlignTop);reference->hide();
        for(int index=0;index<6;++index)row->setRowStretch(index,0);
        row->addWidget(names,0,0,Qt::AlignTop);row->addWidget(info,0,1,Qt::AlignLeft|(summary?Qt::AlignTop:Qt::AlignVCenter));
        if(hero||summary){
            if(action)row->addWidget(action,0,3,Qt::AlignRight|Qt::AlignVCenter);
            row->addWidget(valueLine,1,0,1,3);row->addWidget(change,2,0,1,3,Qt::AlignTop);
            if(narrow||summary)row->addWidget(history,3,0,1,4);else row->addWidget(history,1,3,2,1,Qt::AlignRight|Qt::AlignBottom);
            row->setColumnStretch(2,1);
        }else{
            row->addWidget(valueLine,1,0,1,3);row->addWidget(change,2,0,1,3,Qt::AlignTop);row->setColumnStretch(0,1);
            // Cells in one dashboard row get the tallest cell's height. The spare height goes below
            // the notes, so titles, values and notes line up across cells instead of drifting apart.
            row->setRowStretch(3,1);
        }
        row->invalidate();q->updateGeometry();
    }
    void arrange(bool narrow) {
        if(presentation!=ExploreMetricCard::Presentation::Row){if(arranged&&compact==narrow)return;compact=narrow;arranged=true;arrangeStacked(narrow);return;}
        if(arranged&&compact==narrow)return;compact=narrow;arranged=true;
        for(auto* widget:{names,static_cast<QWidget*>(info),static_cast<QWidget*>(value),static_cast<QWidget*>(unit),static_cast<QWidget*>(freshness),static_cast<QWidget*>(change),static_cast<QWidget*>(history),static_cast<QWidget*>(reference)})row->removeWidget(widget);
        for(int column=0;column<8;++column)row->setColumnStretch(column,0);
        for(int index=0;index<6;++index)row->setRowStretch(index,0);
        change->setWordWrap(narrow);reference->setWordWrap(narrow);
        if(narrow){
            row->addWidget(names,0,0,1,4);row->addWidget(info,0,4,Qt::AlignRight|Qt::AlignTop);
            row->addWidget(value,1,0,1,3,Qt::AlignLeft);row->addWidget(unit,1,3);row->addWidget(freshness,1,4,Qt::AlignRight);
            row->addWidget(change,2,0,1,3);row->addWidget(history,2,3,1,2,Qt::AlignRight);row->setColumnStretch(0,1);
        }else{
            int column=0;for(auto* widget:{names,static_cast<QWidget*>(info),static_cast<QWidget*>(value),static_cast<QWidget*>(unit),static_cast<QWidget*>(freshness),static_cast<QWidget*>(change),static_cast<QWidget*>(history),static_cast<QWidget*>(reference)})row->addWidget(widget,0,column++);
            row->setColumnStretch(0,1);
        }
        row->invalidate();q->updateGeometry();
    }
    void ink() {
        if(data.breadth){
            const auto counts=*data.breadth;
            const auto group=[this](const QString& text,MovementDirection direction){
                return span(text,FontRole::SecondaryStrong14,MovementColorPolicy::colorFor(q,direction));
            };
            value->setTextFormat(Qt::RichText);
            value->setText(group(QString("▲ %1").arg(counts.up),MovementDirection::Up)+" &nbsp; "+
                           group(QString("▼ %1").arg(counts.down),MovementDirection::Down));
            value->setAccessibleName(QObject::tr("상승 %1종목, 하락 %2종목, 보합 %3종목").arg(counts.up).arg(counts.down).arg(counts.flat));
        }
        const auto color=MovementColorPolicy::colorFor(q,data.direction);
        change->setStyleSheet("color: "+color.name()+";");change->ensurePolished();
        auto palette=change->palette();palette.setColor(QPalette::WindowText,color);change->setPalette(palette);change->update();history->update();
    }
    void bindPolicy() {
        auto* next=MovementColorPolicy::forContext(q);
        if(policy==next)return;QObject::disconnect(policyChanged);QObject::disconnect(policyDestroyed);policy=next;
        if(next){policyChanged=QObject::connect(next,&MovementColorPolicy::changed,q,[this]{ink();});
            policyDestroyed=QObject::connect(next,&QObject::destroyed,q,[this]{policy.clear();ink();});}
    }
};
ExploreMetricCard::ExploreMetricCard(QWidget* parent) : QWidget(parent), d_(std::make_unique<Private>()) {
    d_->q=this;setObjectName("exploreMetricCard");setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    auto* rule=new QFrame(this);rule->setObjectName("metricTopRule");rule->setFixedHeight(1);setThemeStyle(rule,"QFrame{background:${rule};border:0;}");layout->addWidget(rule);d_->rule=rule;
    d_->row=new QGridLayout;d_->row->setContentsMargins(0,12,0,12);d_->row->setSpacing(12);layout->addLayout(d_->row);
    d_->names=new QWidget(this);auto* names=new QVBoxLayout(d_->names);names->setContentsMargins(0,0,0,0);names->setSpacing(4);d_->title=label(d_->names,"metricTitle",FontRole::Body15);d_->title->setWordWrap(true);names->addWidget(d_->title);
    d_->freshness=label(d_->names,"metricFreshness",FontRole::Caption12);d_->freshness->setTextFormat(Qt::RichText);d_->freshness->setProperty("type","caption");d_->freshness->setWordWrap(true);d_->freshness->hide();
    d_->info=new InfoButton(this);d_->info->setObjectName("metricInfo");
    d_->value=label(this,"metricValue",FontRole::Value22);d_->value->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    d_->unit=label(this,"metricUnit",FontRole::Number11);
    d_->change=label(this,"metricChange",FontRole::Caption12);d_->change->setTextFormat(Qt::RichText);
    d_->history=new MetricHistoryWidget(this);d_->history->hide();
    d_->reference=label(this,"metricEvidenceReference",FontRole::Number11);setThemeStyle(d_->reference,"QLabel{color:${accentText};}");d_->reference->hide();
    d_->arrange(true);
    d_->bindPolicy();d_->ink();bindThemePresentation(this,[this]{d_->freshness->setText(captionText(compactFreshness(d_->data)));d_->ink();});
}
ExploreMetricCard::~ExploreMetricCard() {QObject::disconnect(d_->policyChanged);QObject::disconnect(d_->policyDestroyed);}
void ExploreMetricCard::setData(const ExploreMetricCardData& data) {
    if(d_->data==data){d_->bindPolicy();d_->ink();return;}
    const QPointer<ExploreMetricCard> alive(this);
    if(d_->data.id!=data.id||d_->data.sourceKey!=data.sourceKey)d_->info->clearPrivate();if(!alive)return;
    d_->data=data;d_->plotted=usableHistory(data);d_->title->setText(usabilityPublicText(data.title));const auto state=compactFreshness(data);d_->freshness->setText(captionText(state));d_->freshness->setVisible(!state.isEmpty());
    const bool numeric=QRegularExpression("\\A[+−-]?[0-9][0-9,.]*(?:[eE][+−-]?[0-9]+)?\\z").match(data.value).hasMatch();
    const bool asciiUnit=QRegularExpression("\\A[\\x20-\\x7e]*\\z").match(data.unit).hasMatch();d_->unit->setFont(KeefiFonts::font(asciiUnit?FontRole::Number11:FontRole::Caption12));d_->unit->setText(usabilityPublicText(data.unit));d_->unit->setVisible(!data.unit.isEmpty());
    d_->numeric=numeric;d_->value->setTextFormat(Qt::PlainText);d_->value->setAccessibleName({});d_->value->setWordWrap(!numeric&&!data.breadth);d_->value->setText(usabilityPublicText(data.value));d_->fitValue();
    const auto marker=MovementColorPolicy::marker(data.direction);
    const auto change=KeefiFonts::mixedTextHtml(data.changeText.isEmpty()?QString{}:(marker.isEmpty()?QString{}:marker+" ")+usabilityPublicText(data.changeText),FontRole::Caption12);
    if(d_->change->text()!=change)d_->change->setText(change);d_->change->setVisible(!data.changeText.isEmpty());
    d_->change->setAccessibleDescription(data.direction==MovementDirection::Up?tr("상승"):data.direction==MovementDirection::Down?tr("하락"):data.direction==MovementDirection::Flat?tr("보합"):tr("방향 미확인"));
    d_->history->setZeroBaseline(data.unit==QStringLiteral("%"));d_->history->setHistory(d_->plotted);d_->history->setVisible(hasConnectedHistory(d_->plotted));d_->value->setToolTip(compactSources(data));d_->info->setInformation(usabilityPublicText(data.title),displayedInformation(data));if(!alive)return;
    d_->bindPolicy();d_->ink();updateGeometry();
}
ExploreMetricCardData ExploreMetricCard::data() const { return d_->data; }
void ExploreMetricCard::setPresentation(Presentation presentation) {
    if(d_->presentation==presentation)return;d_->presentation=presentation;
    const bool hero=presentation==Presentation::Hero,cell=presentation==Presentation::Cell,summary=presentation==Presentation::Summary;
    d_->title->setFont(KeefiFonts::font(hero||summary?FontRole::Title17:cell?FontRole::BodyBold15:FontRole::Body15));
    d_->history->setWide(hero||summary,summary?64:96);d_->rule->setVisible(!cell);
    d_->row->setContentsMargins(0,cell||summary?0:12,0,cell||summary?0:12);d_->row->setVerticalSpacing(hero||summary?6:cell?4:12);
    d_->arranged=false;d_->arrange(width()>0&&width()<640);
    const auto current=d_->data;d_->data={};setData(current);
}
ExploreMetricCard::Presentation ExploreMetricCard::presentation() const { return d_->presentation; }
void ExploreMetricCard::setHeaderAction(QWidget* widget) {
    if(d_->action==widget)return;d_->action=widget;if(widget)widget->setParent(this);
    d_->arranged=false;d_->arrange(width()>0&&width()<640);if(widget)widget->show();
}
void ExploreMetricCard::setHeaderDetail(QWidget* widget) {
    if(d_->headerDetail==widget)return;
    auto* names=static_cast<QVBoxLayout*>(d_->names->layout());
    if(d_->headerDetail){names->removeWidget(d_->headerDetail);d_->headerDetail->hide();}
    d_->headerDetail=widget;
    if(widget){widget->setParent(d_->names);names->addWidget(widget);widget->show();}
    d_->row->invalidate();updateGeometry();
}
void ExploreMetricCard::setInlineState(QWidget* widget) {
    if(d_->state==widget)return;d_->state=widget;if(widget)widget->setParent(this);
    d_->arranged=false;d_->arrange(width()>0&&width()<640);
}
void ExploreMetricCard::setEvidenceNumbers(const QList<int>&) {d_->reference->clear();d_->reference->hide();}
void ExploreMetricCard::clear() {const QPointer<ExploreMetricCard> alive(this);d_->info->clearPrivate();if(alive){setData({});setEvidenceNumbers({});}}
InfoButton* ExploreMetricCard::informationButton() const { return d_->info; }
void ExploreMetricCard::setPrivateAccess(bool allowed) {d_->info->setPrivateAccess(allowed);}
bool ExploreMetricCard::setPrivateInformation(const QString& text) { return d_->info->setPrivateInformation(text); }
void ExploreMetricCard::clearPrivate() {d_->info->clearPrivate();}
bool ExploreMetricCard::historyVisible() const { return !d_->history->isHidden(); }
QList<ExploreHistoryPoint> ExploreMetricCard::plottedHistory() const { return d_->plotted; }
void ExploreMetricCard::showEvent(QShowEvent* event) {QWidget::showEvent(event);d_->bindPolicy();d_->ink();}
void ExploreMetricCard::resizeEvent(QResizeEvent* event) {QWidget::resizeEvent(event);d_->arrange(event->size().width()<(d_->presentation==Presentation::Hero?560:640));d_->fitValue();}
}
