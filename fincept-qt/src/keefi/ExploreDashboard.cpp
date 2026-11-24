#include "keefi/PlainToolTip.h"
#include <QSignalBlocker>
#include "keefi/ExploreDashboard.h"
#include "keefi/SegmentedControlStyle.h"
#include "keefi/TodayCalendarView.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/InfoButton.h"
#include "keefi/CompactControls.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/PortfolioPresentation.h"
#include <QBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QButtonGroup>
#include <QPainter>
#include <QResizeEvent>
#include <QDateTime>
#include <QTimeZone>
#include <QRegularExpression>
#include <QTextLayout>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace keefi {
namespace {
QLabel* label(const QString& text,QWidget* parent,FontRole role=FontRole::Body15,const QString& name={}) {
    auto* l=new QLabel(text,parent);l->setObjectName(name);l->setTextFormat(Qt::PlainText);
    l->setFont(KeefiFonts::font(role));l->setMinimumWidth(0);return l;
}
// Keep the full summary text accessible while drawing one elided line at narrow widths.
class SummaryLabel final : public QLabel {
  public:
    SummaryLabel(const QString& text,QWidget* parent,const QString& name={}) : QLabel(text,parent) {
        setObjectName(name);setTextFormat(Qt::PlainText);setFont(KeefiFonts::font(FontRole::Caption12));
        setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    }
  protected:
    void paintEvent(QPaintEvent* event) override {
        if(wordWrap()){QLabel::paintEvent(event);return;}
        QPainter painter(this);painter.setFont(font());painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(contentsRect(),alignment()|Qt::TextSingleLine,fontMetrics().elidedText(text().simplified(),Qt::ElideRight,contentsRect().width()));
    }
};
QWidget* panel(QWidget* parent,const QString& name) {
    auto* p=new QWidget(parent);p->setObjectName(name);p->setAttribute(Qt::WA_StyledBackground,true);
    p->setMinimumWidth(0);p->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    setThemeStyle(p,"QWidget#"+name+"{background:${bg};border:1px solid ${rule};border-radius:12px;}");return p;
}
QVBoxLayout* column(QWidget* w,int margin=16,int spacing=8) {
    auto* l=new QVBoxLayout(w);l->setContentsMargins(margin,margin,margin,margin);l->setSpacing(spacing);return l;
}
QPushButton* button(const QString& text,QWidget* p) {
    auto* b=new QPushButton(text,p);b->setFont(KeefiFonts::font(FontRole::CaptionStrong12));b->setCursor(Qt::PointingHandCursor);
    setThemeStyle(b,"QPushButton{background:transparent;color:${text2};border:0;padding:4px;min-height:0;}QPushButton:hover,QPushButton:focus{color:${accent};}");return b;
}
QString evidenceText(const ExploreMetricCardData& d) {
    QStringList lines{d.publicInformation};
    for(const auto& e:d.evidence){QString line=e.provider;if(!e.sourceDate.isEmpty())line+=" · "+e.sourceDate;
        if(e.sourceTimeMs>0)line+=" · "+QDateTime::fromMSecsSinceEpoch(e.sourceTimeMs,QTimeZone("Asia/Seoul")).toString("MM/dd HH:mm KST");
        if(!e.state.isEmpty())line+=" · "+e.state;lines.append(line);}
    return lines.join('\n');
}
// A ledger note's width exactly as a row draws it: Caption12 with Number11 digit runs.
int mixedWidth(const QString& text) {
    QTextLayout layout(text,KeefiFonts::font(FontRole::Caption12));layout.setFormats(KeefiFonts::numberRuns(text));
    layout.beginLayout();auto line=layout.createLine();if(line.isValid())line.setLineWidth(1000000);layout.endLayout();
    return line.isValid()?int(std::ceil(line.naturalTextWidth())):0;
}
MovementDirection directionOf(const QString& signedDecimal) {
    bool ok=false;const double value=signedDecimal.toDouble(&ok);
    if(signedDecimal.isEmpty()||!ok||!std::isfinite(value))return MovementDirection::Unknown;
    return value>0?MovementDirection::Up:value<0?MovementDirection::Down:MovementDirection::Flat;
}
QString signedPercent(const QString& value) {
    const auto direction=directionOf(value);if(direction==MovementDirection::Unknown)return {};
    return (direction==MovementDirection::Up&&!value.startsWith('+')?QStringLiteral("+"):QString{})+value+QLatin1Char('%');
}
QString kst(qint64 ms,const QString& format){return QDateTime::fromMSecsSinceEpoch(ms,QTimeZone("Asia/Seoul")).toString(format);}
// No value yet: the row keeps its "—" and says why in the short note; the full
// reason stays in the row's tooltip. Nothing is guessed or carried over.
ExploreMetricCardData unavailableMetric(ExploreMetricCardData d,const QString& failure) {
    d.value=QStringLiteral("—");d.direction=MovementDirection::Unknown;
    d.changeText=failure.isEmpty()?QObject::tr("받는 중"):failure.contains(QObject::tr("공시 전"))?QObject::tr("공시 전"):QObject::tr("확인 필요");
    if(!failure.isEmpty())d.publicInformation+=QStringLiteral("\n")+failure;
    return d;
}
ExploreMetricCardData quoteMetric(const QString& key,const QString& title,const WorldMarketQuote& q,const QString& failure,qint64 now,
                                  const QString& unit,const QString& provider,const QString& information,bool index=false) {
    ExploreMetricCardData d;d.id="today:world:"+key;d.title=title;d.sourceKey="world:"+key;d.publicInformation=information;
    if(!q.available())return unavailableMetric(d,failure);
    d.value=groupedAmount(q.price);d.unit=unit;
    // Pre-market rows repeat the last close with a zero change: say "장 전", not 0.00%.
    const bool preOpen=q.session==WorldMarketSession::PreOpen;
    d.direction=preOpen?MovementDirection::Unknown:directionOf(q.changePercent);d.changeText=preOpen?QObject::tr("장 전"):signedPercent(q.changePercent);
    // Late means this app has not re-read the value in three refresh periods.
    const bool late=now-q.receivedAtMs>3*(index&&q.session==WorldMarketSession::Closed?WorldMarkets::ClosedRefreshMs:WorldMarkets::RefreshMs);
    d.freshnessText=late?QObject::tr("지연"):q.session==WorldMarketSession::Closed?QObject::tr("마감"):QString{};
    const auto kstDate=QDateTime::fromMSecsSinceEpoch(q.tradedAtMs,QTimeZone("Asia/Seoul")).date();
    QStringList lines{QObject::tr("기준 시각 %1 KST%2").arg(kst(q.tradedAtMs,QStringLiteral("yyyy-MM-dd HH:mm")),
        q.localDate.isValid()&&q.localDate!=kstDate?QObject::tr(" (현지 날짜 %1)").arg(q.localDate.toString(QStringLiteral("MM/dd"))):QString{})};
    const auto change=(d.direction==MovementDirection::Up&&!q.change.startsWith('+')?QStringLiteral("+"):QString{})+groupedAmount(q.change);
    if(!preOpen)lines.append(QObject::tr("전일 대비 %1 (%2)").arg(change,d.changeText));
    lines.append(QObject::tr("제공 %1%2").arg(q.delayMinutes>0?QObject::tr("%1분 지연").arg(q.delayMinutes):QObject::tr("실시간"),q.venue.isEmpty()?QString{}:QStringLiteral(" · ")+q.venue));
    lines.append(q.session==WorldMarketSession::Open?QObject::tr("상태 장중"):q.session==WorldMarketSession::Closed?QObject::tr("상태 장 마감 · 마지막 거래일 값")
        :preOpen?QObject::tr("상태 장 시작 전 · 직전 거래일 종가"):QObject::tr("상태 확인 안 됨"));
    if(late)lines.append(QObject::tr("최근 확인 %1 KST").arg(kst(q.receivedAtMs,QStringLiteral("MM/dd HH:mm:ss"))));
    if(!failure.isEmpty())lines.append(QObject::tr("최근 재확인 실패: %1").arg(failure));
    d.publicInformation+=QStringLiteral("\n")+lines.join('\n');
    d.evidence={{d.sourceKey,provider,q.localDate.toString(Qt::ISODate),q.tradedAtMs,q.receivedAtMs,true,
        q.session==WorldMarketSession::Open?QObject::tr("장중"):q.session==WorldMarketSession::Closed?QObject::tr("장 마감"):preOpen?QObject::tr("장 전"):QString{}}};
    return d;
}
}

QList<QPair<QString,ExploreMetricCardData>> todayWorldMarketMetrics(const WorldMarketsSnapshot& s,qint64 now) {
    QList<QPair<QString,ExploreMetricCardData>> out;
    const auto indexInfo=QObject::tr("네이버 증권 공개 시세입니다. 공식 API가 아니어서 형식이 바뀌면 값을 추정하지 않고 '확인 필요'로 표시합니다.");
    const auto bankInfo=QObject::tr("하나은행 고시 환율(매매기준율, 네이버 증권)입니다. 고시 회차마다 바뀌며 은행 간 시장 환율과 조금 다를 수 있어요. 원/100엔은 100엔당 원화입니다.");
    const auto futureInfo=QObject::tr("미국 상품거래소 최근월 선물 가격입니다(네이버 증권). 금·은은 온스당 달러, WTI는 배럴당 달러입니다. 만기 월이 바뀌는 날에는 가격이 계단처럼 움직일 수 있어요.");
    struct Row { const char* key; WorldMarketItem item; const char* unit; };
    const Row rows[]{{"spx",WorldMarketItem::Spx,""},{"ndx",WorldMarketItem::Ndx,""},{"kospi",WorldMarketItem::Kospi,""},
        {"nikkei",WorldMarketItem::Nikkei,""},{"hsi",WorldMarketItem::HangSeng,""},{"ftse",WorldMarketItem::Ftse,""},
        {"usdkrw",WorldMarketItem::UsdKrw,"KRW"},{"jpykrw",WorldMarketItem::JpyKrw,"KRW"},{"cnykrw",WorldMarketItem::CnyKrw,"KRW"},
        {"gold",WorldMarketItem::Gold,"USD/oz"},{"silver",WorldMarketItem::Silver,"USD/oz"},{"wti",WorldMarketItem::Wti,"USD/bbl"}};
    for(const auto& row:rows){
        const auto key=QString::fromLatin1(row.key);const auto item=row.item;
        const bool bank=item==WorldMarketItem::UsdKrw||item==WorldMarketItem::JpyKrw||item==WorldMarketItem::CnyKrw;
        const bool future=item==WorldMarketItem::Gold||item==WorldMarketItem::Silver||item==WorldMarketItem::Wti;
        out.append({key,quoteMetric(key,WorldMarkets::itemName(item),s.quote(item),s.failure(item),now,QString::fromLatin1(row.unit),
            bank?QObject::tr("하나은행 · 네이버 증권"):QObject::tr("네이버 증권"),bank?bankInfo:future?futureInfo:indexInfo,!bank&&!future)});
    }
    // Hana posts KRW per unit; the cross divides two postings. Each leg's own
    // previous posting gives the previous cross, so the change is not invented.
    const auto cross=[&](const QString& key,const QString& title,WorldMarketItem base,const QString& formula){
        const auto value=WorldMarkets::cross(s.quote(base),1,s.quote(WorldMarketItem::JpyKrw),100);
        auto failure=s.failure(base);if(failure.isEmpty())failure=s.failure(WorldMarketItem::JpyKrw);
        return quoteMetric(key,title,value,failure,now,QStringLiteral("JPY"),QObject::tr("하나은행 · 네이버 증권(계산)"),
            QObject::tr("하나은행 고시 원화 환율끼리 나눠 계산한 값입니다(%1). 은행 간 시장 환율과 조금 다를 수 있어요. 전일 대비는 두 환율 각각의 전일 고시값으로 계산합니다.").arg(formula));
    };
    out.append({"usdjpy",cross("usdjpy",QObject::tr("달러/엔"),WorldMarketItem::UsdKrw,QObject::tr("원/달러 ÷ 원/100엔 × 100"))});
    ExploreMetricCardData dxy;dxy.id="today:world:dxy";dxy.title=QObject::tr("달러 인덱스");dxy.sourceKey="world:dxy";
    dxy.publicInformation=QObject::tr("ECB 일별 참고환율에 ICE 달러 인덱스(DXY)와 같은 6개 통화 가중치(유로 57.6%·엔 13.6%·파운드 11.9%·캐나다달러 9.1%·스웨덴 크로나 4.2%·스위스 프랑 3.6%)를 적용한 근사값입니다. ECB가 영업일 하루 한 번(중부유럽 14:15 무렵) 정하는 환율이라 실제 거래되는 달러 인덱스와 조금 다릅니다.\n출처: European Central Bank — https://www.ecb.europa.eu/stats/eurofxref/");
    const auto& index=s.dollarIndex;
    if(!index.available())dxy=unavailableMetric(dxy,s.dollarIndexFailure);
    else{
        dxy.value=index.value;
        if(!index.changePercent.isEmpty()){dxy.direction=directionOf(index.changePercent);dxy.changeText=signedPercent(index.changePercent);}
        else dxy.changeText=QObject::tr("ECB %1").arg(index.date.toString(QStringLiteral("MM/dd")));
        if(now-index.receivedAtMs>3*WorldMarkets::DailyRefreshMs)dxy.freshnessText=QObject::tr("지연");
        dxy.publicInformation+=QObject::tr("\n기준일 %1").arg(index.date.toString(Qt::ISODate));
        if(index.previousDate.isValid())dxy.publicInformation+=QObject::tr(" · 전 영업일 %1의 %2 대비").arg(index.previousDate.toString(Qt::ISODate),index.previousValue);
        if(!s.dollarIndexFailure.isEmpty())dxy.publicInformation+=QObject::tr("\n최근 재확인 실패: %1").arg(s.dollarIndexFailure);
        dxy.evidence={{"world:dxy",QStringLiteral("ECB"),index.date.toString(Qt::ISODate),0,index.receivedAtMs,true,QObject::tr("일별 참고환율")}};
    }
    out.append({"dxy",dxy});
    out.append({"audjpy",cross("audjpy",QObject::tr("호주달러/엔"),WorldMarketItem::AudKrw,QObject::tr("원/호주달러 ÷ 원/100엔 × 100"))});
    ExploreMetricCardData jgb;jgb.id="today:world:jgb";jgb.title=QObject::tr("일본 10년물");jgb.sourceKey="world:jgb";
    jgb.publicInformation=QObject::tr("일본 재무성이 영업일마다 공시하는 국채 10년물 금리입니다. 장중 시세가 아닙니다.\n출처: Japan Ministry of Finance — https://www.mof.go.jp/english/policy/jgbs/reference/interest_rate/");
    const auto& japan=s.japanYield;
    if(!japan.available())jgb=unavailableMetric(jgb,s.japanYieldFailure);
    else{
        jgb.value=japan.tenYear;jgb.unit=QStringLiteral("%");jgb.changeText=japan.date.toString(QStringLiteral("MM/dd"));
        if(now-japan.receivedAtMs>3*WorldMarkets::DailyRefreshMs)jgb.freshnessText=QObject::tr("지연");
        jgb.publicInformation+=QObject::tr("\n기준일 %1 (일본 날짜)").arg(japan.date.toString(Qt::ISODate));
        if(japan.previousDate.isValid())jgb.publicInformation+=QObject::tr(" · 전 영업일 %1 %2%").arg(japan.previousDate.toString(Qt::ISODate),japan.previousTenYear);
        if(!s.japanYieldFailure.isEmpty())jgb.publicInformation+=QObject::tr("\n최근 재확인 실패: %1").arg(s.japanYieldFailure);
        jgb.evidence={{"world:jgb",QObject::tr("일본 재무성"),japan.date.toString(Qt::ISODate),0,japan.receivedAtMs,true,{}}};
    }
    out.append({"jgb",jgb});
    return out;
}

// Only draws source-recorded points. Missing intervals break the line.
class DashboardLine final : public QWidget {
  public:
    explicit DashboardLine(QWidget* p):QWidget(p){setFixedHeight(30);setObjectName("todayPrivateHistory");}
    void setPoints(const QList<PortfolioHistoryPoint>& points){
        points_=points;
        bool previousValid=false,hasSegment=false;
        for(const auto& point:points_){
            bool number=false;const auto value=point.value.toDouble(&number);
            const bool valid=point.available&&number&&std::isfinite(value);
            hasSegment|=previousValid&&valid&&point.connectToPrevious;
            previousValid=valid;
        }
        setVisible(hasSegment);update();
    }
  protected:
    void paintEvent(QPaintEvent*) override {
        if(points_.size()<2)return;double low=INFINITY,high=-INFINITY;for(const auto& p:points_)if(p.available){bool ok=false;double v=p.value.toDouble(&ok);if(ok&&std::isfinite(v)){low=qMin(low,v);high=qMax(high,v);}}
        if(!std::isfinite(low)||points_.last().timeMs<=points_.first().timeMs)return;
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(themeColors().text3,1.5));bool previousValid=false;QPointF previous;
        for(const auto& p:points_){bool ok=false;double value=p.value.toDouble(&ok);if(!p.available||!ok||!std::isfinite(value)){previousValid=false;continue;}
            QPointF next(2.+(width()-4.)*double(p.timeMs-points_.first().timeMs)/double(points_.last().timeMs-points_.first().timeMs),high>low?height()-3.-(height()-6.)*(value-low)/(high-low):height()/2.);
            if(previousValid&&p.connectToPrevious)painter.drawLine(previous,next);previous=next;previousValid=true;}
    }
  private: QList<PortfolioHistoryPoint> points_;
};

// The public metric projection already owns the source and gap validation.
// Preserve its segment flags; never interpolate across missing observations.
class DashboardMetricLine final : public QWidget {
  public:
    explicit DashboardMetricLine(QWidget* parent):QWidget(parent){
        setObjectName("todayGapHistory");setFixedHeight(22);
        setAccessibleName(tr("김프 최근 24시간 실제 관측 기록"));
        bindThemePresentation(this,[this]{update();});
    }
    bool setData(const ExploreMetricCardData& data){
        points_.clear();const auto& history=data.history;
        if(!history.available||data.sourceKey.isEmpty()||history.sourceKey!=data.sourceKey||data.unit!="%"||history.unit!=data.unit
            ||history.points.size()<2||history.points.size()>ExploreMetricCard::MaxHistoryPoints){update();return false;}
        qint64 previous=0;bool connected=false;
        for(const auto& point:history.points){
            if(point.atMs<=previous||!std::isfinite(point.value)){update();return false;}
            connected|=previous>0&&point.connectsPrevious;previous=point.atMs;
        }
        if(connected)points_=history.points;update();return connected;
    }
  protected:
    void paintEvent(QPaintEvent*) override {
        if(points_.size()<2)return;
        const auto [low,high]=std::minmax_element(points_.cbegin(),points_.cend(),[](const auto& a,const auto& b){return a.value<b.value;});
        const auto span=double(points_.last().atMs-points_.first().atMs);const auto range=high->value-low->value;
        const auto at=[&](const ExploreHistoryPoint& point){return QPointF(2.+(width()-4.)*double(point.atMs-points_.first().atMs)/span,
            range>0?height()-3.-(height()-6.)*(point.value-low->value)/range:height()/2.);};
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(themeColors().text3,1.5));
        for(int i=1;i<points_.size();++i)if(points_[i].connectsPrevious)painter.drawLine(at(points_[i-1]),at(points_[i]));
    }
  private:
    QList<ExploreHistoryPoint> points_;
};

TodayLedgerRow::TodayLedgerRow(QWidget* parent):QWidget(parent) {
    setObjectName("todayLedgerRow");setMinimumWidth(0);setFixedHeight(RowHeight);
    auto* line=new QHBoxLayout(this);line->setContentsMargins(0,0,0,0);line->setSpacing(LedgerSpacing);
    // The name gives way first; the value and the fixed-width trail keep every row's
    // numbers on the same right edges.
    title_=label({},this,FontRole::Secondary14,"metricTitle");title_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    state_=label({},this,FontRole::Caption12,"metricState");state_->hide();
    value_=label({},this,FontRole::Number11,"metricValue");value_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    trail_=label({},this,FontRole::Caption12,"metricChange");trail_->setTextFormat(Qt::RichText);trail_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    line->addWidget(title_,1);line->addWidget(state_);line->addWidget(value_);line->addWidget(trail_);
    bindThemePresentation(this,[this]{ink();});
}
void TodayLedgerRow::setTrailWidth(int width){trail_->setFixedWidth(qMax(0,width));}
void TodayLedgerRow::showEvent(QShowEvent* event){
    QWidget::showEvent(event);
    auto* policy=MovementColorPolicy::forContext(this);
    if(policy_!=policy){
        if(policy_)disconnect(policy_,nullptr,this,nullptr);
        policy_=policy;if(policy)connect(policy,&MovementColorPolicy::changed,this,[this]{ink();});
    }
    ink();
}
void TodayLedgerRow::setData(const ExploreMetricCardData& d){
    if(data_==d){ink();return;}
    data_=d;
    const auto title=usabilityPublicText(d.title);title_->setText(title);
    const bool late=d.freshnessText==QStringLiteral("지연"),closed=d.freshnessText==QStringLiteral("마감");
    // A late market move is not shown as if current: its change column says "지연".
    // A late basis note (a date) stays visible, so the state mark says it instead.
    const bool move=d.direction!=MovementDirection::Unknown;
    const bool lateMark=late&&!move;
    state_->setText(lateMark?tr("지연"):closed&&!late?QStringLiteral("●"):QString{});state_->setVisible(lateMark||(closed&&!late));
    state_->setAccessibleName(lateMark?tr("지연"):closed?tr("장 마감"):QString{});
    static const QRegularExpression numeric(QStringLiteral("\\A[+−-]?[0-9][0-9,.]*\\z"));
    const bool number=numeric.match(d.value).hasMatch();
    const bool inlineUnit=d.unit==QStringLiteral("%")||d.unit==QStringLiteral("%p");
    value_->setFont(KeefiFonts::font(number?FontRole::Number11:FontRole::Caption12));
    value_->setText(number?d.value+(inlineUnit?d.unit:QString{}):usabilityPublicText(d.value));
    // Sign and the movement colour carry direction, as in a watchlist; no ▲/▼ column.
    const auto change=usabilityPublicText(d.changeText);const auto shown=late&&move?tr("지연"):change;
    trail_->setText(shown.isEmpty()?QString{}:KeefiFonts::mixedTextHtml(shown,FontRole::Caption12));
    const auto movement=d.direction==MovementDirection::Up?tr("상승"):d.direction==MovementDirection::Down?tr("하락"):d.direction==MovementDirection::Flat?tr("보합"):QString{};
    QStringList summary{value_->text()+(number&&!inlineUnit&&!d.unit.isEmpty()?QLatin1Char(' ')+d.unit:QString{})};
    if(!change.isEmpty())summary.append(movement.isEmpty()?change:movement+QLatin1Char(' ')+change);
    if(closed)summary.append(tr("장 마감 · 마지막 거래일 값"));
    if(late)summary.append(tr("지연 · 최근 확인값"));
    setAccessibleName(title);setAccessibleDescription(summary.join(QStringLiteral(", ")));
    const auto details=evidenceText(d).trimmed();
    setToolTip(plainToolTip(title+QStringLiteral("  ")+summary.join(QStringLiteral(" · "))+(details.isEmpty()?QString{}:QStringLiteral("\n\n")+details)));
    ink();
}
void TodayLedgerRow::ink(){
    const auto& colors=themeColors();const bool late=data_.freshnessText==QStringLiteral("지연");
    const auto paint=[](QLabel* target,const QColor& color){
        const auto style=QStringLiteral("QLabel{color:%1;}").arg(color.name());if(target->styleSheet()!=style)target->setStyleSheet(style);};
    paint(title_,colors.text);paint(state_,colors.text3);paint(value_,late?colors.text3:colors.text);
    const auto direction=data_.direction;
    paint(trail_,late||direction==MovementDirection::Unknown?colors.text3:direction==MovementDirection::Flat?colors.text2:MovementColorPolicy::colorFor(this,direction));
}

ExploreTodayView::ExploreTodayView(QWidget* parent):QWidget(parent) {
    setObjectName("todayDashboard");setMinimumWidth(0);
    auto* root=column(this,0,16);
    top_=new QWidget(this);top_->setObjectName("todaySummaryHeader");top_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Maximum);
    auto* header=column(top_,0,8);
    emptyStrip_=panel(top_,"todayEmptyStrip");emptyStrip_->setFixedHeight(40);auto* strip=new QHBoxLayout(emptyStrip_);strip->setContentsMargins(12,1,12,1);strip->setSpacing(16);
    const auto emptySlot=[&](const QString& title,int stretch){
        auto* slot=new QWidget(emptyStrip_);slot->setMinimumWidth(0);auto* line=new QHBoxLayout(slot);line->setContentsMargins(0,0,0,0);line->setSpacing(8);
        line->addWidget(label(title,slot,FontRole::CaptionStrong12));strip->addWidget(slot,stretch);return slot;
    };
    emptyPrivate_=emptySlot(tr("오늘 내 자산"),38);emptyNotice_=emptySlot(tr("거래소 공지"),37);
    header->addWidget(emptyStrip_);
    summaryCards_=new QWidget(top_);summaryCards_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);
    auto* top=new QHBoxLayout(summaryCards_);top->setContentsMargins(0,0,0,0);top->setSpacing(16);
    // A layout strut is a floor. A taller three-row notice still gets its full
    // intrinsic height when the dashboard shares a short window with the ledger.
    top->addStrut(144);header->addWidget(summaryCards_);
    privatePanel_=panel(summaryCards_,"todayPrivatePanel");auto* personal=column(privatePanel_,12,6);auto* head=new QHBoxLayout;privateHeading_=head;
    head->addWidget(label(tr("오늘 내 자산"),privatePanel_,FontRole::BodyBold15));head->addStretch();
    privateInfo_=new InfoButton(privatePanel_);privateInfo_->setInformation(tr("오늘 내 자산"),tr("전일 대비는 어제 마지막으로 함께 평가한 원화 자산 총액을 기준으로 합니다. 표시한 시각은 한국 시각이며, 평가액 변화에는 입출금이 포함될 수 있어요. 아래 선은 오늘의 관측 기록입니다."));head->addWidget(privateInfo_);personal->addLayout(head);
    placeholder_=new SummaryLabel(tr("금고를 열면 오늘 내 자산 변화가 여기에 보여요"),emptyPrivate_,"todayPrivatePlaceholder");placeholder_->setToolTip(placeholder_->text());static_cast<QHBoxLayout*>(emptyPrivate_->layout())->addWidget(placeholder_,1);
    unlock_=button(tr("금고 열기"),emptyPrivate_);unlock_->setObjectName("todayUnlock");connect(unlock_,&QPushButton::clicked,this,&ExploreTodayView::vaultOpenRequested);emptyPrivate_->layout()->addWidget(unlock_);
    privateBody_=new QWidget(privatePanel_);auto* numbers=column(privateBody_,0,3);auto* amount=new QHBoxLayout;
    total_=label({},privateBody_,FontRole::Value22,"todayPrivateTotal");unit_=label({},privateBody_,FontRole::Caption12);amount->addWidget(total_);amount->addWidget(unit_);amount->addStretch();numbers->addLayout(amount);
    change_=label({},privateBody_,FontRole::Caption12,"todayPrivateChange");numbers->addWidget(change_);privateLine_=new DashboardLine(privateBody_);numbers->addWidget(privateLine_);personal->addWidget(privateBody_);personal->addStretch();
    contributionPanel_=panel(summaryCards_,"todayContributionPanel");auto* contribution=column(contributionPanel_,12,4);contribution->addWidget(label(tr("어디서 움직였나"),contributionPanel_,FontRole::BodyBold15));
    contributionBasis_=label({},contributionPanel_,FontRole::Caption12,"todayContributionBasis");contribution->addWidget(contributionBasis_);
    for(int i=0;i<3;++i){auto* text=label({},contributionPanel_,FontRole::Caption12,"todayContributionValue");contribution->addWidget(text);contributionLabels_.append(text);auto* bar=new QProgressBar(contributionPanel_);bar->setRange(0,1000);bar->setTextVisible(false);bar->setFixedHeight(3);contribution->addWidget(bar);contributionBars_.append(bar);}contribution->addStretch();
    noticePanel_=panel(summaryCards_,"todayNoticePanel");auto* notice=column(noticePanel_);auto* noticeHead=new QHBoxLayout;noticeHead->addWidget(label(tr("거래소 공지"),noticePanel_,FontRole::BodyBold15));noticeHead->addStretch();noticeMore_=button(tr("다음 →"),noticePanel_);noticeHead->addWidget(noticeMore_);notice->addLayout(noticeHead);
    noticeStatus_=new SummaryLabel(tr("받는 중"),noticePanel_);noticeStatus_->setWordWrap(true);notice->addWidget(noticeStatus_);noticeRows_=new QVBoxLayout;noticeRows_->setSpacing(2);notice->addLayout(noticeRows_);notice->addStretch();
    connect(noticeMore_,&QPushButton::clicked,this,[this]{noticePage_=(noticePage_+1)%qMax(1,(int(notices_.size())+2)/3);renderNotices();});
    top->addWidget(privatePanel_,38);top->addWidget(contributionPanel_,25);top->addWidget(noticePanel_,37);root->addWidget(top_);
    metricPanel_=panel(this,"todayMetricPanel");auto* indicators=column(metricPanel_,12,4);auto* metricHead=new QHBoxLayout;
    metricHead->addWidget(label(tr("지금 볼 지표"),metricPanel_,FontRole::BodyBold15));metricHead->addStretch();
    auto* closedLegend=label(tr("● 장 마감"),metricPanel_,FontRole::Caption12,"todayClosedLegend");setThemeStyle(closedLegend,"QLabel{color:${text3};}");
    closedLegend->setToolTip(tr("회색 점은 장이 닫혀 마지막 거래일 값을 보여 준다는 뜻이에요."));metricHead->addWidget(closedLegend);metricHead->addSpacing(8);
    auto* modes=new QButtonGroup(this);modes->setExclusive(true);
    for(const auto& pair:{qMakePair(QString("morning"),tr("아침")),qMakePair(QString("korea"),tr("국장")),qMakePair(QString("night"),tr("밤"))}){auto* b=button(pair.second,metricPanel_);b->setObjectName("todayMode_"+pair.first);b->setCheckable(true);b->setAutoExclusive(true);b->setChecked(pair.first==mode_);applySegmentedControlStyle(b,24);modes->addButton(b);metricHead->addWidget(b);connect(b,&QPushButton::toggled,this,[this,pair](bool selected){if(selected){mode_=pair.first;layoutMetrics();}});}
    auto* alerts=button(tr("지표 알림 설정"),metricPanel_);connect(alerts,&QPushButton::clicked,this,&ExploreTodayView::alertSettingsRequested);metricHead->addWidget(alerts);
    indicators->addLayout(metricHead);
    auto* headlineLine=new QHBoxLayout;headlineLine->setSpacing(12);
    headline_=label(tr("공개 시장 정보를 받고 있어요"),metricPanel_,FontRole::Secondary14,"todayHeadline");headline_->setWordWrap(false);headline_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);headline_->setFixedHeight(40);headlineLine->addWidget(headline_,3);
    gapStrip_=new QWidget(metricPanel_);gapStrip_->setObjectName("todayGapHistoryStrip");gapStrip_->setFixedSize(200,40);auto stripPolicy=gapStrip_->sizePolicy();stripPolicy.setRetainSizeWhenHidden(true);gapStrip_->setSizePolicy(stripPolicy);
    auto* gapLayout=column(gapStrip_,0,2);auto* gapCaption=label({},gapStrip_,FontRole::Caption12);
    gapCaption->setTextFormat(Qt::RichText);gapCaption->setText(KeefiFonts::mixedTextHtml(tr("김프 24시간"),FontRole::Caption12));gapLayout->addWidget(gapCaption);
    gapLine_=new DashboardMetricLine(gapStrip_);gapLayout->addWidget(gapLine_);gapStrip_->hide();headlineLine->addWidget(gapStrip_);
    indicators->addLayout(headlineLine);
    // Four ledgers side by side, each a fixed list of aligned rows (TradingView-style):
    // a value never moves because a neighbour's note is longer or still loading.
    metrics_=new QGridLayout;metrics_->setContentsMargins(0,0,0,0);metrics_->setHorizontalSpacing(20);metrics_->setVerticalSpacing(12);
    // Each ledger's width is measured from its widest name, value and note, so no
    // column is given less than its rows need. widestValue uses the 11px number face.
    // Rows carry their display names from the start: before any source answers, a row
    // reads "S&P 500 — 받는 중", never an internal key.
    struct Group {
        QString id, title, summary;
        PublicSourceInformation source;
        QString details;
        QList<QPair<QString,QString>> rows;
        QString widestNote, widestTitle, widestValue;
        bool closedDot;
    };
    const QList<Group> groups{
        {"rates",tr("김프·금리"),tr("회색 글씨는 기준일·고시 시각이에요."),
            {tr("김프 · 미국·일본 10년물 · 미·일 금리차"),
             tr("업비트 · 바이낸스 · 하나은행 · 미국 재무부 · 일본 재무성"),
             tr("미국·일본 10년물은 장중 시세가 아닙니다."),tr("% · %p"),
             tr("줄에 마우스를 올리면 출처와 확인 시각이 보여요.")},tr("BTC 김프: 업비트 BTC 원화가 ÷ (바이낸스 BTCUSDT × 하나은행 원/달러 고시 환율) − 1.\n"
            "테더 김프: 업비트 USDT 원화가 ÷ 하나은행 원/달러 고시 환율 − 1. 1 USDT ≈ 1 USD 전제입니다.\n"
            "미국 10년물: 미국 재무부 일별 금리. 일본 10년물: 일본 재무성 일별 국채 금리. 둘 다 장중 시세가 아닙니다.\n"
            "미·일 금리차: 미국 10년물 − 일본 10년물. '엔 캐리' 거래의 기본 수익원이라, 이 차이가 빠르게 줄면 청산 압력이 커질 수 있어요.\n"
            "회색 글씨는 기준일·고시 시각이에요. 줄에 마우스를 올리면 출처와 확인 시각이 보여요."),
            {{"gap",tr("BTC 김프")},{"tether",tr("테더 김프")},{"treasury",tr("미국 10년물")},{"jgb",tr("일본 10년물")},{"spread",tr("미·일 금리차")}},
            QStringLiteral("11/04·11/05"),tr("미·일 금리차"),QStringLiteral("2.045%p"),false},
        {"indices",tr("세계 지수"),tr("등락은 직전 거래일 종가 대비입니다."),
            {tr("등락은 직전 거래일 종가 대비입니다."),tr("네이버 증권 공개 시세입니다."),
             tr("제공처가 밝힌 지연 시간(실시간·15분 등)은 줄에 마우스를 올리면 보여요."),tr("지수 · %"),
             tr("회색 점(●)은 장이 닫혀 마지막 거래일 값을 보여 준다는 뜻이에요.")},
            tr("공식 API가 아니어서 형식이 바뀌면 값을 추정하지 않고 '확인 필요'로 표시합니다. 아침·국장·밤 버튼은 지수 순서를 바꿉니다."),
            {{"spx",QStringLiteral("S&P 500")},{"ndx",tr("나스닥 100")},{"kospi",tr("코스피")},{"nikkei",tr("닛케이 225")},{"hsi",tr("항셍")},{"ftse",QStringLiteral("FTSE 100")}},
            QStringLiteral("+10.00%"),tr("나스닥 100"),QStringLiteral("61,077.90"),true},
        {"fx",tr("환율"),tr("은행 간 시장 환율과 조금 다를 수 있어요."),
            {tr("원/달러·원/100엔·원/위안: 하나은행 고시 환율"),tr("하나은행 (네이버 증권) · ECB"),
             tr("달러 인덱스는 하루 한 번 바뀝니다."),tr("원 · 엔 · 달러 인덱스"),
             tr("은행 간 시장 환율과 조금 다를 수 있어요.")},tr(
            "달러/엔·호주달러/엔: 하나은행 원화 환율끼리 나눠 계산한 값입니다. 엔 캐리 거래가 청산될 때 가장 먼저 크게 움직이는 축이에요.\n"
            "달러 인덱스: ECB 일별 참고환율에 ICE 달러 인덱스의 6개 통화 가중치를 적용한 근사값으로, 하루 한 번 바뀝니다."),
            {{"usdkrw",tr("원/달러")},{"jpykrw",tr("원/100엔")},{"cnykrw",tr("원/위안")},{"usdjpy",tr("달러/엔")},{"dxy",tr("달러 인덱스")},{"audjpy",tr("호주달러/엔")}},
            QStringLiteral("+10.00%"),tr("호주달러/엔"),QStringLiteral("1,344.90"),true},
        {"commodities",tr("원자재"),tr("미국 상품거래소 최근월 선물 가격"),
            {tr("미국 상품거래소 최근월 선물 가격"),tr("네이버 증권"),tr("제공처 기준 10분 지연"),
             tr("금·은은 온스당 달러, WTI는 배럴당 달러"),
             tr("만기 월이 바뀌는 날에는 가격이 계단처럼 움직일 수 있어요.")},{},
            {{"gold",tr("금")},{"silver",tr("은")},{"wti",tr("WTI 원유")}},QStringLiteral("+10.00%"),tr("WTI 원유"),QStringLiteral("4,137.00"),true}};
    for(const auto& group:groups){
        auto* box=new QWidget(metricPanel_);box->setObjectName("todayGroup_"+group.id);box->setMinimumWidth(0);
        auto* column=new QVBoxLayout(box);column->setContentsMargins(0,0,0,0);column->setSpacing(0);
        auto* head=new QWidget(box);head->setFixedHeight(CompactButtonHeight);
        auto* headGrid=new QGridLayout(head);headGrid->setContentsMargins(0,0,0,0);
        auto* headLine=new QHBoxLayout;headLine->setSpacing(4);headGrid->addLayout(headLine,0,0);
        auto* title=label(group.title,head,FontRole::CaptionStrong12,"todayGroupTitle_"+group.id);setThemeStyle(title,"QLabel{color:${text2};}");
        headLine->addWidget(title);headLine->addStretch();
        auto* info=new InfoButton(head);info->setObjectName("todayGroupInfo_"+group.id);info->setPublicSourceInformation(group.title,group.summary,group.source,group.details);headLine->addWidget(info);
        column->addWidget(head);
        auto* rule=new QFrame(head);rule->setFixedHeight(1);rule->setAttribute(Qt::WA_TransparentForMouseEvents);
        setThemeStyle(rule,"QFrame{background:${rule};border:0;}");headGrid->addWidget(rule,0,0,Qt::AlignBottom);rule->lower();
        auto* list=new QVBoxLayout;list->setContentsMargins(0,0,0,0);list->setSpacing(0);column->addLayout(list);
        const int trail=mixedWidth(group.widestNote)+4;
        const int dot=group.closedDot?QFontMetrics(KeefiFonts::font(FontRole::Caption12)).horizontalAdvance(QStringLiteral("●"))+TodayLedgerRow::LedgerSpacing:0;
        const int need=QFontMetrics(KeefiFonts::font(FontRole::Secondary14)).horizontalAdvance(group.widestTitle)+dot
            +QFontMetrics(KeefiFonts::font(FontRole::Number11)).horizontalAdvance(group.widestValue)+trail+2*TodayLedgerRow::LedgerSpacing+2;
        // Decides columns and stretch only: a minimum width here would stop the page from narrowing.
        box->setProperty("ledgerNeed",need);
        for(const auto& [key,name]:group.rows){
            auto* row=new TodayLedgerRow(box);row->setObjectName("todayMetric_"+key);row->setTrailWidth(trail);list->addWidget(row);rows_.insert(key,row);
            ExploreMetricCardData waiting;waiting.title=name;waiting.value=QStringLiteral("—");waiting.changeText=tr("받는 중");row->setData(waiting);
        }
        box->setFixedHeight(CompactButtonHeight+int(group.rows.size())*TodayLedgerRow::RowHeight);
        if(group.id=="indices")indexRows_=list;
        groups_.append(box);
    }
    indicators->addLayout(metrics_,1);
    root->addWidget(metricPanel_);
    scheduleRow_=new QWidget(this);scheduleRow_->setObjectName("todayScheduleRow");auto* schedules=new QHBoxLayout(scheduleRow_);schedules->setContentsMargins(0,0,0,0);schedules->setSpacing(16);
    calendar_=new TodayCalendarView(scheduleRow_);schedules->addWidget(calendar_,1);root->addWidget(scheduleRow_,1);layoutMetrics();clearPrivate();
}
ExploreTodayView::~ExploreTodayView(){clearPrivate();}
QWidget* ExploreTodayView::takeSummaryHeader(){
    if(top_)layout()->removeWidget(top_);return top_;
}
void ExploreTodayView::setWideLayout(bool wide){
    if(wideLayout_==wide)return;wideLayout_=wide;layoutSummary();
}
void ExploreTodayView::layoutSummary(){
    if(!top_)return;
    const bool hasPrivate=!privateBody_->isHidden();
    bool hasContribution=false;for(auto* value:contributionLabels_)hasContribution|=!value->isHidden();
    const bool hasNotices=!notices_.isEmpty()||noticeRefreshInPlace_;
    emptyPrivate_->setVisible(!hasPrivate);emptyNotice_->setVisible(!hasNotices);
    placeholder_->setVisible(!hasPrivate);unlock_->setVisible(!hasPrivate);
    auto* infoHost=hasPrivate?privatePanel_:emptyPrivate_;
    if(privateInfo_->parentWidget()!=infoHost){
        if(hasPrivate)privateHeading_->addWidget(privateInfo_);else emptyPrivate_->layout()->addWidget(privateInfo_);
        privateInfo_->show();
    }
    auto* statusHost=hasNotices?noticePanel_:emptyNotice_;
    if(noticeStatus_->parentWidget()!=statusHost){
        if(hasNotices)static_cast<QVBoxLayout*>(noticePanel_->layout())->insertWidget(1,noticeStatus_);
        else static_cast<QHBoxLayout*>(emptyNotice_->layout())->addWidget(noticeStatus_,1);
    }
    noticeStatus_->setWordWrap(hasNotices);noticeStatus_->setToolTip(noticeStatus_->text());noticeStatus_->setVisible(!noticeStatus_->text().isEmpty());
    auto* noticeHost=wideLayout_?scheduleRow_:summaryCards_;
    if(noticePanel_->parentWidget()!=noticeHost)static_cast<QHBoxLayout*>(noticeHost->layout())->addWidget(noticePanel_,wideLayout_?1:37);
    privatePanel_->setVisible(hasPrivate);contributionPanel_->setVisible(hasContribution);noticePanel_->setVisible(hasNotices);
    emptyStrip_->setVisible(!hasPrivate||!hasNotices);
    summaryCards_->setVisible(hasPrivate||hasContribution||(!wideLayout_&&hasNotices));
    // The surrounding page owns header visibility after takeSummaryHeader().
    top_->updateGeometry();
}
void ExploreTodayView::layoutMetrics(){
    // 아침: last night's US close first. 국장: Asia first. 밤: Europe and the US first.
    if(indexRows_){
        const QStringList order=mode_=="korea"?QStringList{"kospi","nikkei","hsi","spx","ndx","ftse"}
            :mode_=="night"?QStringList{"ftse","spx","ndx","kospi","nikkei","hsi"}:QStringList{"spx","ndx","kospi","nikkei","hsi","ftse"};
        for(const auto& key:order)indexRows_->removeWidget(rows_[key]);
        for(const auto& key:order)indexRows_->addWidget(rows_[key]);
    }
    // Four ledgers share one line when every measured column fits (a 1440px window
    // gives 918px); narrower pages show two per line and scroll instead of squeezing.
    const int content=width()-24,gap=metrics_->horizontalSpacing();
    QList<int> needs;for(auto* group:groups_)needs<<group->property("ledgerNeed").toInt();
    const int all=std::accumulate(needs.cbegin(),needs.cend(),0)+gap*(int(needs.size())-1);
    const int pairs=qMax(needs.value(0)+needs.value(1),needs.value(2)+needs.value(3))+gap;
    metricColumns_=content>=all?4:content>=pairs?2:1;
    for(auto* group:groups_)metrics_->removeWidget(group);
    for(int i=0;i<groups_.size();++i)metrics_->addWidget(groups_[i],i/metricColumns_,i%metricColumns_,Qt::AlignTop);
    // Spare width goes to each column in proportion to what it needs.
    for(int i=0;i<4;++i)metrics_->setColumnStretch(i,i<metricColumns_?(metricColumns_==4?needs.value(i):1):0);
    int grid=0;const int lines=(int(groups_.size())+metricColumns_-1)/metricColumns_;
    for(int line=0;line<lines;++line){int tallest=0;for(int i=line*metricColumns_;i<qMin(int(groups_.size()),(line+1)*metricColumns_);++i)tallest=qMax(tallest,groups_[i]->maximumHeight());grid+=tallest;}
    grid+=metrics_->verticalSpacing()*(lines-1);
    // 24px panel margins + 22px heading + 40px summary + two 4px gaps. Fixed, so
    // asynchronous values and notes never move the schedule below.
    metricPanel_->setFixedHeight(94+grid);
}
void ExploreTodayView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    const bool compact = height() < 760;
    // Size against the actual client viewport, after the native title bar,
    // app controls and loading line. Preserve private/metric typography and
    // let the schedule paginate fewer rows before the whole page must scroll.
    // 208px still fits its status, both date groups, two 32px rows and pager.
    if (auto* root = qobject_cast<QVBoxLayout*>(layout())) root->setSpacing(compact ? 8 : 16);
    calendar_->setMinimumHeight(compact ? 208 : 240);
    setHeadline(headline_->toolTip());
    layoutMetrics();
}
void ExploreTodayView::setMetric(const QString& key,const ExploreMetricCardData& value){
    auto* row=rows_.value(key);if(!row)return;
    if(key=="gap")gapStrip_->setVisible(gapLine_->setData(value));
    auto data=value;data.history={};row->setData(data);
}
void ExploreTodayView::setHeadline(const QString& text){
    const auto line=text.simplified();
    headline_->setText(headline_->fontMetrics().elidedText(line,Qt::ElideRight,headline_->width()));headline_->setToolTip(line);headline_->setAccessibleName(line);
}
void ExploreTodayView::clearPrivate(){
    if(!top_)return;
    total_->clear();unit_->clear();change_->clear();privateLine_->setPoints({});privateBody_->hide();placeholder_->show();unlock_->show();
    change_->hide();contributionBasis_->clear();contributionBasis_->setToolTip({});for(auto* l:contributionLabels_){l->clear();l->hide();}for(auto* b:contributionBars_){b->setValue(0);b->hide();}
    privateInfo_->clearPrivate();layoutSummary();
}
void ExploreTodayView::setPrivateSummary(bool allowed,const QString& amount,const QString& unit,const PortfolioDayHistory& history,const PortfolioMoneyDisplay& display){
    if(!top_)return;
    if(!allowed){clearPrivate();return;}placeholder_->hide();unlock_->hide();privateBody_->show();total_->setText(amount);unit_->setText(unit);total_->setFont(KeefiFonts::font(decimal::valid(QString(amount).remove(','))?FontRole::Value22:FontRole::Body15));
    privateInfo_->setPrivateAccess(true);
    const auto& yesterday=history.previousDay.comparison;
    const auto percent=yesterday.available && yesterday.complete
        ? PortfolioMoneyDisplay::changePercent(yesterday.difference,yesterday.openingValue) : QString{};
    change_->setText(percent.isEmpty()?QString{}:tr("어제 %1 대비 %2").arg(kst(yesterday.fromMs,"HH:mm"),percent));
    change_->setVisible(!percent.isEmpty());
    privateLine_->setPoints(history.points);

    // Prefer the same observed period as the headline. If turnover prevents
    // attributing that total change, don't imply today's groups explain it.
    const auto& contributions=yesterday.available?history.previousDay:history.contributions;
    const auto& basis=yesterday.available?yesterday:history.comparison;
    QList<PortfolioContributionGroup> moved;
    if(contributions.available && basis.available)
        for(const auto& group:contributions.groups)
            if(!group.difference.isEmpty() && group.difference!="0") moved.append(group);
    const auto magnitude=[](const QString& value){return value.startsWith('-')?value.mid(1):value;};
    std::stable_sort(moved.begin(),moved.end(),[&](const auto& a,const auto& b){
        return decimal::compare(magnitude(a.difference),magnitude(b.difference))>0;
    });
    const auto period=yesterday.available?tr("어제 %1 이후"):tr("오늘 %1 이후");
    const auto basisText=period.arg(kst(basis.fromMs,"HH:mm"))+
        (basis.complete?QString{}:tr(" · 부분합"))+(moved.size()>contributionLabels_.size()?tr(" · 상위 3개"):QString{});
    contributionBasis_->setText(moved.isEmpty()?QString{}:basisText);
    contributionBasis_->setToolTip(moved.isEmpty()?QString{}:basisText+tr(" (KST) · 평가액 증감, 입출금 포함 가능"));
    for(int i=0;i<contributionLabels_.size();++i){
        auto* text=contributionLabels_[i];auto* bar=contributionBars_[i];
        const bool shown=i<moved.size();
        text->setVisible(shown);bar->setVisible(shown);
        if(!shown){text->clear();bar->setValue(0);continue;}
        const auto& group=moved[i];
        const bool down=group.difference.startsWith('-');
        text->setText(portfolioContributionClassLabel(group.assetClass)+"  "+(down?"▼ ":"▲ +")+display.amount(group.difference)+" "+display.unit());
        bar->setValue(portfolioShareTenths(magnitude(group.difference),magnitude(moved.first().difference)));
        const auto color=MovementColorPolicy::fillFor(this,down?MovementDirection::Down:MovementDirection::Up).name();
        const auto style=QStringLiteral("QProgressBar{border:0;background:${surface};}QProgressBar::chunk{background:%1;}").arg(color);
        if(bar->property("keefiThemeTemplate").toString()!=style)setThemeStyle(bar,style);
    }
    layoutSummary();
}
void ExploreTodayView::setNoticeFeeds(const QList<OfficialNoticeFeed>& feeds){
    if(!top_)return;
    QList<QPair<QString,OfficialNotice>> rows;bool loading=false,failed=false;
    for(const auto& feed:feeds){loading|=feed.loading;failed|=!feed.loading&&!feed.available&&!feed.reason.isEmpty();if(!feed.available)continue;for(const auto& n:feed.notices){auto words=(n.categories.join(' ')+" "+n.title).toLower();words.remove(' ');bool relevant=false;
        for(const auto& token:{"delist","caution","거래지원종료","거래유의","입출금중단","입출금일시중단","입출금일시중지","입금중단","출금중단","입금일시중단","출금일시중단","네트워크업그레이드","거래정지"})if(words.contains(QString::fromUtf8(token)))relevant=true;
        relevant|=(words.contains("suspend")||words.contains("suspension")||words.contains("halt"))&&(words.contains("deposit")||words.contains("withdraw"));relevant|=words.contains("network")&&words.contains("upgrade");if(!relevant)continue;rows.append({OfficialNotices::sourceName(feed.source),n});}}
    QString signature=QString::number(loading)+QString::number(failed);for(const auto& row:rows)signature+=row.first+row.second.url+row.second.title+row.second.categories.join('|')+QString::number(row.second.publishedAtMs)+row.second.publishedText+row.second.modifiedText;if(signature==noticeSignature_)return;noticeSignature_=signature;
    // The feed clears its rows while it refreshes. Keep the existing card's
    // footprint until the replies settle, without retaining the old notices.
    const bool keepCard=rows.isEmpty()&&loading&&(!notices_.isEmpty()||noticeRefreshInPlace_);
    if(keepCard!=noticeRefreshInPlace_){
        noticeRefreshInPlace_=keepCard;
        noticePanel_->setMinimumHeight(keepCard&&!wideLayout_&&noticePanel_->isVisible()?noticePanel_->height():0);
    }
    std::stable_sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.second.publishedAtMs>b.second.publishedAtMs;});notices_=rows;noticePage_=qMin(noticePage_,qMax(0,(int(rows.size())-1)/3));
    noticeStatus_->setText(rows.isEmpty()?(loading?tr("받는 중"):failed?tr("일부 공지를 받지 못했어요"):tr("관련 공지가 없어요")):failed?tr("일부 거래소 공지를 받지 못했어요"):QString{});noticeStatus_->setVisible(!noticeStatus_->text().isEmpty());renderNotices();
}
void ExploreTodayView::renderNotices(){
    while(auto* item=noticeRows_->takeAt(0)){delete item->widget();delete item;}
    for(int i=noticePage_*3;i<qMin(int(notices_.size()),noticePage_*3+3);++i){const auto& item=notices_[i];auto* row=new QWidget(noticePanel_);auto* l=new QHBoxLayout(row);l->setContentsMargins(0,0,0,0);l->setSpacing(4);
        auto words=(item.second.categories.join(' ')+" "+item.second.title).toLower();words.remove(' ');QString category;
        if(words.contains("delist")||words.contains("거래지원종료"))category=tr("거래지원 종료");else if(words.contains("caution")||words.contains("유의"))category=tr("거래 유의");else if(words.contains("upgrade")||words.contains("업그레이드"))category=tr("업그레이드");else category=tr("입출금 중단");
        // The notice card changes width when a contribution is shown. Keep its
        // public source and timestamp on separate lines at every width so the
        // header below the tabs keeps the same height through a history read.
        auto* lines=new QVBoxLayout;lines->setContentsMargins(0,0,0,0);lines->setSpacing(1);l->addLayout(lines,1);
        auto* text=new SummaryLabel(item.first+" · "+category,row);text->setToolTip(text->text());lines->addWidget(text);
        const bool absoluteTime=item.second.publishedAtMs>0;
        auto* timestamp=new SummaryLabel(absoluteTime?tr("게시 %1 KST · UTC+9").arg(kst(item.second.publishedAtMs,QStringLiteral("MM/dd HH:mm"))):tr("게시 %1 · 시간대 미제공").arg(item.second.publishedText),row);
        timestamp->setFont(KeefiFonts::font(FontRole::Number11));timestamp->setToolTip(timestamp->text());lines->addWidget(timestamp);
        const auto published=absoluteTime?tr("최초 게시 %1 KST · UTC+9").arg(kst(item.second.publishedAtMs,QStringLiteral("yyyy-MM-dd HH:mm")))
            :tr("원문 게시 %1 · 시간대 미제공").arg(item.second.publishedText);
        QStringList details{item.second.title,item.first,item.second.url,published};
        if(!item.second.modifiedText.isEmpty())details.append(tr("원문 수정 %1 · 시간대 미제공").arg(item.second.modifiedText));
        auto* info=new InfoButton(row);info->setInformation(tr("거래소 공지"),details.join('\n'));l->addWidget(info);noticeRows_->addWidget(row);}
    noticeMore_->setVisible(notices_.size()>3);layoutSummary();
}

ExploreCoinSummary::ExploreCoinSummary(QWidget* parent):QWidget(parent){
    setObjectName("exploreCoinSummary");setAttribute(Qt::WA_StyledBackground,true);setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    setThemeStyle(this,"QWidget#exploreCoinSummary{background:${bg};border:1px solid ${rule};border-radius:12px;}");auto* root=column(this,16,10);root->setAlignment(Qt::AlignTop);auto* tabs=new QHBoxLayout;
    for(int i=0;i<3;++i){auto* b=button({},this);b->setCheckable(true);b->setAutoExclusive(true);tabs->addWidget(b);tabs_.append(b);connect(b,&QPushButton::toggled,this,[this,b](bool selected){if(selected)emit coinSelected(b->property("asset").toString());});}tabs->addStretch();root->addLayout(tabs);
    quote_=new ExploreMetricCard(this);quote_->setObjectName("exploreSelectedCoinQuote");quote_->setPresentation(ExploreMetricCard::Presentation::Summary);quote_->setMinimumHeight(0);root->addWidget(quote_);
    holdingRow_=new QWidget(this);auto* holdingLine=new QHBoxLayout(holdingRow_);holdingLine->setContentsMargins(0,0,0,0);holdingLine->setSpacing(4);
    holding_=label({},holdingRow_,FontRole::Caption12,"exploreSelectedHolding");holding_->setWordWrap(false);holding_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);holdingLine->addWidget(holding_,1);
    holdingInfo_=new InfoButton(holdingRow_);holdingInfo_->setInformation(tr("내 보유"),tr("열린 금고에서 현재 확인한 보유 평가액입니다."));holdingLine->addWidget(holdingInfo_);quote_->setHeaderDetail(holdingRow_);
    marketSupport_=label({},this,FontRole::Caption12,"exploreSummaryMarketSupport");marketSupport_->setWordWrap(true);root->addWidget(marketSupport_);marketSupport_->hide();
    relativeRow_=new QWidget(this);auto* relativeLayout=new QHBoxLayout(relativeRow_);relativeLayout->setContentsMargins(0,0,0,0);
    relative_=label({},relativeRow_,FontRole::Caption12,"exploreRelativeStrength");relative_->setWordWrap(true);relativeLayout->addWidget(relative_,1);
    relativeInfo_=new InfoButton(relativeRow_);relativeLayout->addWidget(relativeInfo_);root->addWidget(relativeRow_);relativeRow_->hide();
    auto* chart=button(tr("차트에서 보기 →"),this);chart->setObjectName("exploreSummaryChart");tabs->addWidget(chart,0,Qt::AlignRight);connect(chart,&QPushButton::clicked,this,&ExploreCoinSummary::chartRequested);clearPrivate();
}
void ExploreCoinSummary::setRelativeStrength(const QString& text,const QString& information){relative_->setText(text);relativeInfo_->setInformation(tr("BTC 대비 변화"),information);relativeRow_->setVisible(!text.isEmpty());}
void ExploreCoinSummary::setCoins(const QStringList& coins,const QString& selected){
    const bool selectedInPresets=coins.mid(0,tabs_.size()).contains(selected);
    for(int i=0;i<tabs_.size();++i){
        auto* b=tabs_[i];const QSignalBlocker block(b);
        // An exclusive group cannot uncheck its last button normally. A coin
        // outside these shortcuts must not leave a different coin underlined.
        if(!selectedInPresets){b->setAutoExclusive(false);b->setChecked(false);b->setAutoExclusive(true);}
        b->setVisible(i<coins.size());if(i>=coins.size())continue;
        b->setText(coins[i]);b->setProperty("asset",coins[i]);b->setChecked(coins[i]==selected);
        setThemeStyle(b,"QPushButton{border:0;background:transparent;color:${text3};padding:4px;}QPushButton:checked{color:${accent};border-bottom:2px solid ${accent};}");
    }
}
void ExploreCoinSummary::setQuote(const ExploreMetricCardData& data){quote_->setData(data);}
void ExploreCoinSummary::setMetrics(const ExploreMetricCardData& gap,const ExploreMetricCardData&,const ExploreMetricCardData&,const QString&){
    // The owning window still uses these public metrics for other views and alerts.
    // This summary retains only the original market-support projection.
    marketSupport_->setText(gap.changeText.contains(tr("업비트 원화마켓 미지원"))?tr("업비트 원화마켓 미지원"):QString{});
    marketSupport_->setVisible(!marketSupport_->text().isEmpty());
}
void ExploreCoinSummary::setPrivateHolding(bool allowed,const QString& text){if(!allowed){clearPrivate();return;}holding_->setText(text.isEmpty()?tr("이 코인은 보유하지 않아요"):text);holding_->show();holdingInfo_->setPrivateAccess(true);holdingInfo_->setPrivateInformation(text);holdingInfo_->show();holdingRow_->show();}
void ExploreCoinSummary::clearPrivate(){holding_->clear();holding_->hide();holdingInfo_->clearPrivate();holdingInfo_->hide();holdingRow_->hide();quote_->clearPrivate();}
}
