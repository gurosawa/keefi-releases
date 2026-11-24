#include "keefi/MarketMetricsView.h"
#include "keefi/CoinbaseQuotes.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/ExploreMetricCard.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/LocalCheckBox.h"
#include "keefi/PortfolioPresentation.h"
#include <QDateTime>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QLabel>
#include <QPushButton>
#include <QGridLayout>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QShowEvent>
#include <QTimer>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QFrame>
#include <QPainter>
#include <QPainterPath>

namespace keefi {
namespace {
QString timeText(qint64 at) {
    return at > 0 ? QDateTime::fromMSecsSinceEpoch(at).toTimeZone(QTimeZone("Asia/Seoul")).toString("MM-dd HH:mm:ss") + " KST" : "원천 시각 미제공";
}
QString ratePercent(QString rate) {
    const bool negative = rate.startsWith('-');
    if (negative) rate.remove(0, 1);
    const auto percent = decimal::multiply(rate, "100");
    return percent.isEmpty() ? QString{} : (negative && percent != "0" ? "−" : "") + percent;
}
QString evidenceText(const QString& title, const AssetEvidence& e) {
    return title + "\n" + e.source + "\n" + e.url + "\n관측 " + timeText(e.sourceTimeMs) + " · 수신 " + timeText(e.receivedAtMs);
}
QString freshness(const AssetEvidence& e){return e.receivedAtMs>0?QObject::tr("수신 %1").arg(timeText(e.receivedAtMs)):QObject::tr("아직 수신하지 않음");}
QString gapFreshness(const ExchangePriceGap& gap){qint64 received=0;for(const auto& leg:gap.legs)received=qMax(received,leg.evidence.receivedAtMs);return received>0?QObject::tr("최근 수신 %1").arg(timeText(received)):QObject::tr("공개 시세 확인 대기");}
QColor stateColor(MarketFlowState state){const auto& c=themeColors();return state==MarketFlowState::Hot?c.accentText:state==MarketFlowState::Cool?c.koreanDownText:c.text2;}
void paintState(QLabel* label,MarketFlowState state){label->setStyleSheet(QStringLiteral("QLabel{color:%1;}").arg(stateColor(state).name()));}
// One display sentence from public states only. Copy templates live in UsabilityPresentation.
QString todaySentence(MarketFlowState gap,MarketFlowState funding,MarketFlowState coinbase,const QString& calendar){
    const QList<QPair<QString,MarketFlowState>> items{{usabilityText(UsabilityText::FlowMetricKoreaGap),gap},{usabilityText(UsabilityText::FlowMetricFunding),funding},
        {usabilityText(UsabilityText::FlowSummaryCoinbase).arg(QString{}).trimmed(),coinbase}};
    const auto join=[](const QStringList& names){QString text;for(int i=0;i<names.size();++i){if(i==0)text=names[i];else if(i==names.size()-1)text=usabilityParticle(text,QStringLiteral("과"),QStringLiteral("와"))+" "+names[i];else text+=", "+names[i];}return text;};
    QStringList hot,cool,normal;for(const auto& [name,state]:items){if(state==MarketFlowState::Hot)hot<<name;else if(state==MarketFlowState::Cool)cool<<name;else if(state==MarketFlowState::Normal)normal<<name;}
    QStringList parts;
    if(!hot.isEmpty())parts<<usabilityText(UsabilityText::FlowSentenceHot).arg(usabilityParticle(join(hot),QStringLiteral("이"),QStringLiteral("가")));
    if(!cool.isEmpty())parts<<usabilityText(UsabilityText::FlowSentenceCool).arg(usabilityParticle(join(cool),QStringLiteral("은"),QStringLiteral("는")));
    if(!normal.isEmpty())parts<<usabilityText(UsabilityText::FlowSentenceNormal).arg(usabilityParticle(join(normal),QStringLiteral("은"),QStringLiteral("는")));
    if(!calendar.isEmpty())parts<<calendar;
    return parts.isEmpty()?usabilityText(UsabilityText::FlowSummaryChecking):parts.join(' ');
}
// A checkable alert control drawn as the bordered bell button of the market flow mockup.
class FlowAlertToggle final : public QCheckBox {
  public:
    explicit FlowAlertToggle(QWidget* parent):QCheckBox(parent){
        setFont(KeefiFonts::font(FontRole::Secondary14));setCursor(Qt::PointingHandCursor);sync();
        connect(this,&QCheckBox::toggled,this,[this]{sync();});bindThemePresentation(this,[this]{update();});
    }
    QSize sizeHint()const override{return QSize(12+15+6+QFontMetrics(font()).horizontalAdvance(text())+12,36);}
    QSize minimumSizeHint()const override{return sizeHint();}
  protected:
    bool hitButton(const QPoint& point)const override{return rect().contains(point);}
    void paintEvent(QPaintEvent*)override{
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto& c=themeColors();const bool on=isChecked();const auto ink=on?c.text:c.text2;
        p.setPen(QPen(hasFocus()?c.accent:on?c.text:c.rule,1));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5),8,8);
        QPainterPath bell;bell.moveTo(4,11);bell.lineTo(4,7.4);bell.arcTo(QRectF(4,3.4,8,8),180,-180);bell.lineTo(12,11);bell.lineTo(13.2,12.3);bell.lineTo(2.8,12.3);bell.closeSubpath();
        bell.moveTo(6.6,14);bell.arcTo(QRectF(6.6,12.6,2.8,2.8),180,180);
        p.save();p.translate(12,(height()-15)/2.0);p.scale(15.0/16.0,15.0/16.0);p.setPen(QPen(ink,1.4,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(bell);p.restore();
        p.setPen(ink);p.setFont(font());p.drawText(QRectF(33,0,width()-45,height()),Qt::AlignVCenter|Qt::AlignLeft,text());
    }
  private:
    void sync(){setText(usabilityText(isChecked()?UsabilityText::FlowAlertOn:UsabilityText::FlowAlertEnable));updateGeometry();}
};
QFrame* ruleLine(QWidget* parent){auto* rule=new QFrame(parent);setThemeStyle(rule,"QFrame{background:${rule};border:0;}");return rule;}
}
MarketMetricsView::MarketMetricsView(MarketMetrics* metrics, QWidget* parent) : MarketMetricsView(metrics, nullptr, nullptr, parent) {}
MarketMetricsView::MarketMetricsView(MarketMetrics* metrics, PublicQuoteStream* upbit, PublicQuoteStream* binance, QWidget* parent)
    : QWidget(parent), metrics_(metrics), upbit_(upbit), binance_(binance) {
    setObjectName("marketMetricsView");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(30);
    auto* today=new QWidget(this);today->setObjectName("marketMetricsToday");auto* todayLayout=new QVBoxLayout(today);todayLayout->setContentsMargins(0,0,0,0);todayLayout->setSpacing(6);
    title_=new QLabel(today);title_->setObjectName("marketMetricsTitle");title_->setTextFormat(Qt::PlainText);title_->setFont(KeefiFonts::font(FontRole::CaptionStrong12));setThemeStyle(title_,"QLabel{color:${text3};}");todayLayout->addWidget(title_);
    summary_=new QLabel(today);summary_->setObjectName("marketMetricsSummary");summary_->setWordWrap(true);summary_->setTextFormat(Qt::PlainText);summary_->setFont(KeefiFonts::font(FontRole::Lead28));
    summary_->setMaximumWidth(680);summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);todayLayout->addWidget(summary_);layout->addWidget(today);
    const auto card=[&](const char* name,ExploreMetricCard::Presentation presentation){auto* value=new ExploreMetricCard(this);value->setObjectName(name);value->setMinimumWidth(0);value->setPresentation(presentation);return value;};
    const auto row=[&](ExploreMetricCard* value,MarketFlowMetric metric,const char* prefix,UsabilityText meaning,bool hero){
        MetricPresentation result;result.row=new QWidget(this);result.row->setObjectName(QString::fromLatin1(prefix)+"Row");
        auto* body=new QVBoxLayout(result.row);body->setContentsMargins(hero?0:18,hero?0:16,hero?0:18,hero?0:18);body->setSpacing(hero?10:8);body->addWidget(value);
        result.state=new QLabel(result.row);result.state->setObjectName(QString::fromLatin1(prefix)+(metric==MarketFlowMetric::OpenInterest?"Flow":"State"));result.state->setTextFormat(Qt::PlainText);
        result.state->setFont(KeefiFonts::font(metric==MarketFlowMetric::OpenInterest?FontRole::Secondary14:hero?FontRole::Title17:FontRole::BodyBold15));result.state->setWordWrap(metric==MarketFlowMetric::OpenInterest);
        if(metric==MarketFlowMetric::OpenInterest)body->addWidget(result.state);else value->setInlineState(result.state);
        result.meaning=new QLabel(usabilityText(meaning),result.row);result.meaning->setObjectName(QString::fromLatin1(prefix)+"Meaning");result.meaning->setWordWrap(true);result.meaning->setTextFormat(Qt::PlainText);
        result.meaning->setFont(KeefiFonts::font(FontRole::Secondary14));setThemeStyle(result.meaning,"QLabel{color:${text2};}");body->addWidget(result.meaning);
        result.alert=new FlowAlertToggle(result.row);result.alert->setObjectName(QString::fromLatin1(prefix)+"Alert");result.alert->setAccessibleName(marketFlowMetricLabel(metric)+" · "+usabilityText(UsabilityText::FlowAlertEnable));
        if(hero)value->setHeaderAction(result.alert);else body->addWidget(result.alert,0,Qt::AlignLeft);
        if(!hero)body->addStretch(1);
        connect(result.alert,&QCheckBox::toggled,this,[this,metric](bool enabled){emit metricAlertToggled(metric,enabled);});return result;
    };
    gap_=card("marketMetricsGapCard",ExploreMetricCard::Presentation::Hero);gapPresentation_=row(gap_,MarketFlowMetric::KoreaGap,"marketMetricsGap",UsabilityText::FlowKoreaGapMeaning,true);layout->addWidget(gapPresentation_.row);
    auto* section=new QWidget(this);section->setObjectName("marketMetricsDerivativesSection");auto* sectionLayout=new QVBoxLayout(section);sectionLayout->setContentsMargins(0,0,0,0);sectionLayout->setSpacing(10);
    auto* sectionRule=ruleLine(section);sectionRule->setFixedHeight(1);sectionLayout->addWidget(sectionRule);
    auto* header=new QHBoxLayout;header->setContentsMargins(0,4,0,0);header->setSpacing(12);
    auto* derivativesTitle=new QLabel(usabilityText(UsabilityText::FlowDerivativesTitle),section);derivativesTitle->setObjectName("marketMetricsDerivativesTitle");derivativesTitle->setTextFormat(Qt::PlainText);derivativesTitle->setFont(KeefiFonts::font(FontRole::Title17));header->addWidget(derivativesTitle);
    derivativesNote_=new QLabel(usabilityText(UsabilityText::FlowDerivativesAutoNote),section);derivativesNote_->setObjectName("marketMetricsDerivativesNote");derivativesNote_->setTextFormat(Qt::PlainText);derivativesNote_->setFont(KeefiFonts::font(FontRole::Caption12));
    setThemeStyle(derivativesNote_,"QLabel{color:${text3};}");derivativesNote_->hide();header->addWidget(derivativesNote_);header->addStretch(1);
    derivativesToggle_=new QPushButton(section);derivativesToggle_->setObjectName("marketMetricsDerivativesToggle");derivativesToggle_->setCheckable(true);derivativesToggle_->setFont(KeefiFonts::font(FontRole::Secondary14));derivativesToggle_->setCursor(Qt::PointingHandCursor);
    setThemeStyle(derivativesToggle_,"QPushButton{border:0;border-radius:0;background:transparent;color:${text2};text-decoration:underline;padding:8px 0;text-align:right;}");header->addWidget(derivativesToggle_);
    sectionLayout->addLayout(header);
    derivativesSummary_=new QLabel(section);derivativesSummary_->setObjectName("marketMetricsDerivativesSummary");derivativesSummary_->setTextFormat(Qt::PlainText);derivativesSummary_->setWordWrap(true);derivativesSummary_->setFont(KeefiFonts::font(FontRole::Secondary14));
    setThemeStyle(derivativesSummary_,"QLabel{color:${text2};}");sectionLayout->addWidget(derivativesSummary_);
    derivatives_=new QWidget(section);derivatives_->setObjectName("marketMetricsDerivatives");cards_=new QGridLayout(derivatives_);cards_->setContentsMargins(0,0,0,0);cards_->setSpacing(0);sectionLayout->addWidget(derivatives_);derivatives_->hide();
    cardsRule_=ruleLine(derivatives_);for(auto*& separator:separators_)separator=ruleLine(derivatives_);
    layout->addWidget(section);
    connect(derivativesToggle_,&QPushButton::toggled,this,[this](bool open){derivatives_->setVisible(open);syncDerivativesHeader();});
    coinbaseGap_=card("marketMetricsCoinbaseGapCard",ExploreMetricCard::Presentation::Cell);funding_=card("marketMetricsFundingCard",ExploreMetricCard::Presentation::Cell);oi_=card("marketMetricsOpenInterestCard",ExploreMetricCard::Presentation::Cell);
    fundingPresentation_=row(funding_,MarketFlowMetric::Funding,"marketMetricsFunding",UsabilityText::FlowPublishedFundingMeaning,false);
    coinbasePresentation_=row(coinbaseGap_,MarketFlowMetric::CoinbaseGap,"marketMetricsCoinbaseGap",UsabilityText::FlowCoinbaseGapMeaning,false);
    oiPresentation_=row(oi_,MarketFlowMetric::OpenInterest,"marketMetricsOpenInterest",UsabilityText::FlowOiMeaning,false);layoutCards();syncDerivativesHeader();
    evidence_=new ExploreEvidenceFooter(this);evidence_->setObjectName("marketMetricsEvidenceFooter");layout->addWidget(evidence_);layout->addStretch(1);
    if (metrics_) connect(metrics_, &MarketMetrics::changed, this, &MarketMetricsView::refresh);
    auto* timer = new QTimer(this); timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this] { if (isVisible() || alertDemandActive_ || backgroundActive_) refresh(); }); timer->start();
    refresh();
}
MarketMetricsView::~MarketMetricsView() {
    if(coinbase_)disconnect(coinbase_,nullptr,this,nullptr);if(upbit_)disconnect(upbit_,nullptr,this,nullptr);if(binance_)disconnect(binance_,nullptr,this,nullptr);if(metrics_)disconnect(metrics_,nullptr,this,nullptr);
    if(coinbase_)coinbase_->setAsset({});
    if (upbit_) upbit_->setMarkets({});
    if (binance_) binance_->setMarkets({});
    if (metrics_) metrics_->setAsset({});
}
void MarketMetricsView::setCoinbaseQuotes(CoinbaseQuotes* model){
    if(coinbase_==model)return;
    if(coinbase_){disconnect(coinbase_,nullptr,this,nullptr);coinbase_->setAsset({});}
    coinbase_=model;if(model)connect(model,&CoinbaseQuotes::changed,this,&MarketMetricsView::refresh);
    // Only an explicit HTTP 404 establishes that the product is unlisted.
    if(model)connect(model,&CoinbaseQuotes::quoteObserved,this,[this](const QString& base){
        if(base!=asset_||!coinbase_)return;coinbaseMissing_=coinbase_->unlisted(base);refresh();});
    updateSubscriptions();refresh();
}
void MarketMetricsView::setPrivateContext(bool unlocked, bool hasFutures) {
    hasFutures = unlocked && hasFutures;
    if (contextUnlocked_ == unlocked && contextHasFutures_ == hasFutures) return;
    const bool locked = contextUnlocked_ && !unlocked;
    contextUnlocked_ = unlocked; contextHasFutures_ = hasFutures;
    if (locked || hasFutures) derivativesToggle_->setChecked(hasFutures);
    syncDerivativesHeader();
}
void MarketMetricsView::syncDerivativesHeader(){
    if(!derivativesToggle_)return;const bool open=derivativesToggle_->isChecked();
    derivativesToggle_->setText(usabilityText(open?UsabilityText::FlowCollapse:UsabilityText::FlowExpand));
    derivativesToggle_->setAccessibleName(usabilityText(UsabilityText::FlowDerivativesTitle)+" "+derivativesToggle_->text());
    derivativesNote_->setVisible(open&&contextHasFutures_);derivativesSummary_->setVisible(!open&&!derivativesSummary_->text().isEmpty());
}
void MarketMetricsView::setPreviousGap(const MarketFlowHistoricalGap& gap) {
    if(previousGap_.available==gap.available&&previousGap_.percent==gap.percent&&previousGap_.comparisonCloseMs==gap.comparisonCloseMs&&previousGap_.receivedAtMs==gap.receivedAtMs&&previousGap_.asset==gap.asset&&previousGap_.series==gap.series)return;
    previousGap_=gap;refresh();
}
MarketMetricsView::MetricPresentation& MarketMetricsView::presentation(MarketFlowMetric metric) {
    switch(metric){case MarketFlowMetric::Funding:return fundingPresentation_;case MarketFlowMetric::CoinbaseGap:return coinbasePresentation_;case MarketFlowMetric::OpenInterest:return oiPresentation_;case MarketFlowMetric::KoreaGap:return gapPresentation_;}
    return gapPresentation_;
}
void MarketMetricsView::setMetricAlertEnabled(MarketFlowMetric metric, bool enabled) {
    auto* box=presentation(metric).alert;if(!box||box->isChecked()==enabled)return;const QSignalBlocker blocker(box);box->setChecked(enabled);
}
void MarketMetricsView::setAlertDemandActive(bool active) {
    if(alertDemandActive_==active)return;
    alertDemandActive_=active;updateSubscriptions();refresh();
}
void MarketMetricsView::setBackgroundActive(bool active) {
    if(backgroundActive_==active)return;
    backgroundActive_=active;updateSubscriptions();refresh();
}
void MarketMetricsView::setListing(Listing upbitKrw, Listing binanceSpot) {
    if(upbitListing_==upbitKrw&&binanceListing_==binanceSpot)return;
    upbitListing_=upbitKrw;binanceListing_=binanceSpot;updateSubscriptions();refresh();
}
bool MarketMetricsView::streamsActive() const { return metrics_ && (isVisible() || alertDemandActive_ || backgroundActive_) && !asset_.isEmpty(); }
QPair<int, int> MarketMetricsView::progress() const {
    if(asset_.isEmpty()||!streamsActive())return {0,0};
    const auto now=QDateTime::currentMSecsSinceEpoch();const bool late=activeSinceMs_>0&&now-activeSinceMs_>=LateMs;
    const auto binance=quotes_.value("public:binance:"+asset_+"USDT");
    const bool gap=MarketMetrics::exchangePriceGap(quotes_.value("public:upbit:KRW-"+asset_),binance,quotes_.value("public:upbit:KRW-USDT"),now).available;
    const auto cb=coinbase_?coinbase_->quotes(now):QMap<QString,AssetQuote>{};
    const bool cbGap=CoinbaseQuotes::priceGap(cb.value(asset_),binance,cb.value("USDT"),now).available;
    const auto d=metrics_?metrics_->derivatives(now):PublicDerivativesMetrics{};const bool matching=d.symbol==asset_+"USDT";
    const auto answered=[&](const QString& value,const QString& reason){return matching&&(!value.isEmpty()||(!reason.isEmpty()&&reason!=MarketMetrics::pendingReason()));};
    const bool unlisted=upbitListing_==Listing::Missing||binanceListing_==Listing::Missing;
    int done=0;
    done+=gap||unlisted||late;
    done+=answered(d.fundingRate,d.fundingReason)||late;
    done+=answered(d.openInterest,d.openInterestReason)||late;
    done+=cbGap||coinbaseMissing_||binanceListing_==Listing::Missing||!coinbase_||late;
    return {done,4};
}
void MarketMetricsView::setCalendarSummary(const QString& text) {
    const auto next=usabilityPublicText(text).trimmed();if(calendarSummary_==next)return;calendarSummary_=next;refresh();
}
void MarketMetricsView::setAsset(const QString& asset) {
    const auto next = QRegularExpression("\\A[A-Z0-9]{1,20}\\z").match(asset).hasMatch() ? asset : QString{};
    if (asset_ == next) return;
    asset_ = next; quotes_.clear(); coinbaseMissing_ = false; activeSinceMs_ = 0;
    upbitListing_ = binanceListing_ = Listing::Unknown; updateSubscriptions(); refresh();
}
void MarketMetricsView::showEvent(QShowEvent* e) { QWidget::showEvent(e); updateSubscriptions(); refresh(); }
void MarketMetricsView::hideEvent(QHideEvent* e) { QWidget::hideEvent(e); updateSubscriptions(); }
void MarketMetricsView::resizeEvent(QResizeEvent* e){QWidget::resizeEvent(e);layoutCards();fitSummary();}
void MarketMetricsView::fitSummary(){
    // The wrapped 28px headline keeps every line inside the scrolling page.
    if(!summary_)return;const auto available=qMin(summary_->maximumWidth(),qMax(1,width()));
    const auto needed=summary_->heightForWidth(available);if(needed>0&&summary_->minimumHeight()!=needed)summary_->setMinimumHeight(needed);
}
void MarketMetricsView::layoutCards(){
    if(!cards_)return;const int columns=width()>=600?3:1;if(columns==columns_)return;columns_=columns;
    while(auto* item=cards_->takeAt(0))delete item;for(int column=0;column<5;++column)cards_->setColumnStretch(column,0);
    cardsRule_->setFixedHeight(1);cards_->addWidget(cardsRule_,0,0,1,columns==3?5:1);
    const QList<QWidget*> rows{fundingPresentation_.row,coinbasePresentation_.row,oiPresentation_.row};
    for(int i=0;i<rows.size();++i){
        if(columns==3){cards_->addWidget(rows[i],1,i*2);cards_->setColumnStretch(i*2,1);
            if(i<2){auto* rule=separators_[i];rule->setMinimumSize(1,0);rule->setMaximumSize(1,QWIDGETSIZE_MAX);rule->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Expanding);cards_->addWidget(rule,1,i*2+1);}}
        else{cards_->addWidget(rows[i],1+i*2,0);
            if(i<2){auto* rule=separators_[i];rule->setMinimumSize(0,1);rule->setMaximumSize(QWIDGETSIZE_MAX,1);rule->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);cards_->addWidget(rule,2+i*2,0);}}
    }
}
void MarketMetricsView::updateSubscriptions() {
    const bool active = streamsActive();
    if (!active) activeSinceMs_ = 0; else if (activeSinceMs_ == 0) activeSinceMs_ = QDateTime::currentMSecsSinceEpoch();
    if (active && !upbit_) {
        upbit_ = new PublicQuoteStream(PublicQuoteStream::Provider::Upbit, this);
        binance_ = new PublicQuoteStream(PublicQuoteStream::Provider::Binance, this);
    }
    if (active && upbit_ && binance_ && !streamsConnected_) {
        streamsConnected_ = true;
        const auto receive = [this](const QList<PublicTick>& ticks) {
            for (const auto& tick : ticks) quotes_[tick.quote.evidence.scope] = tick.quote;
            refresh();
        };
        connect(upbit_, &PublicQuoteStream::ticks, this, receive); connect(binance_, &PublicQuoteStream::ticks, this, receive);
        const auto stateChanged = [this](const QString& prefix) {
            for (auto it = quotes_.begin(); it != quotes_.end();) {
                if (it.key().startsWith(prefix)) it = quotes_.erase(it); else ++it;
            }
            refresh();
        };
        connect(upbit_, &PublicQuoteStream::changed, this, [stateChanged] { stateChanged("public:upbit:"); });
        connect(binance_, &PublicQuoteStream::changed, this, [stateChanged] { stateChanged("public:binance:"); });
    }
    if (upbit_) {
        // Unlisted markets are not requested; KRW-USDT stays for the Binance and Coinbase bridges.
        QStringList markets = active && koreaGapDemandEnabled_ ? QStringList{"KRW-USDT"} : QStringList{};
        if (active && koreaGapDemandEnabled_ && upbitListing_ != Listing::Missing) markets.prepend("KRW-" + asset_);
        markets.removeDuplicates();
        upbit_->setMarkets(markets); binance_->setMarkets(active && asset_ != "USDT" && binanceListing_ != Listing::Missing ? QStringList{asset_ + "USDT"} : QStringList{});
    }
    if (metrics_) metrics_->setAsset(active ? asset_ : QString{});
    if(coinbase_){
        if(!active)coinbase_->setAsset({});
        else if(!coinbaseDemandQueued_){
            // View construction and navigation can briefly present a different market.
            // Resolve demand after the UI transition; a passive FX display never initiates a read.
            coinbaseDemandQueued_=true;
            QTimer::singleShot(0,this,[this]{coinbaseDemandQueued_=false;if(coinbase_&&streamsActive())coinbase_->setAsset(asset_);});
        }
    }
    if (!active) quotes_.clear();
}
void MarketMetricsView::setEvidenceStart(int start){start=qMax(1,start);if(evidenceStart_==start)return;evidenceStart_=start;refreshEvidence();}
int MarketMetricsView::evidenceCount()const{return evidence_?evidence_->evidenceCount():0;}
void MarketMetricsView::refreshEvidence(){
    if(!evidence_)return;const auto previous=evidence_->evidenceCount();QList<ExploreMetricEvidence> entries;
    for(const auto* card:{gap_,coinbaseGap_,funding_,oi_})entries.append(card->data().evidence);
    evidence_->setEntries(entries,evidenceStart_);const auto numbers=evidence_->numbers();int offset=0;
    for(auto* card:{gap_,coinbaseGap_,funding_,oi_}){QList<int> refs;for(int i=0;i<card->data().evidence.size();++i)refs.append(numbers.value(offset++));card->setEvidenceNumbers(refs);}
    if(previous!=evidence_->evidenceCount())emit evidenceChanged();
}
void MarketMetricsView::refresh() {
    if (asset_.isEmpty()) {
        emit snapshotReady(asset_,{},{},{});title_->setText(usabilityText(UsabilityText::FlowTodayTitle));summary_->clear();summary_->hide();gapPresentation_.row->hide();derivativesToggle_->parentWidget()->hide();derivatives_->hide();
        for(auto* card:{gap_,coinbaseGap_,funding_,oi_}){card->clear();card->hide();}refreshEvidence();return;
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto gap = MarketMetrics::exchangePriceGap(quotes_.value("public:upbit:KRW-" + asset_),
        quotes_.value("public:binance:" + asset_ + "USDT"), quotes_.value("public:upbit:KRW-USDT"), now);
    const auto cb=coinbase_?coinbase_->quotes(now):QMap<QString,AssetQuote>{};
    const auto cbGap=CoinbaseQuotes::priceGap(cb.value(asset_),quotes_.value("public:binance:"+asset_+"USDT"),cb.value("USDT"),now);
    const auto d = metrics_ ? metrics_->derivatives(now) : PublicDerivativesMetrics{};
    const bool matching = d.symbol == asset_ + "USDT";
    emit snapshotReady(asset_,gap,cbGap,matching?d:PublicDerivativesMetrics{});
    // Hidden opt-in observations feed the public alert engine without painting a hidden page.
    if(!isVisible()&&!mirroredPresentation_)return;
    title_->setText(usabilityText(UsabilityText::FlowTodayTitle));gapPresentation_.row->show();derivativesToggle_->parentWidget()->show();derivatives_->setVisible(derivativesToggle_->isChecked());summary_->show();
    const auto gapState=gap.available?marketFlowState(MarketFlowMetric::KoreaGap,gap.percent):MarketFlowState::Unavailable;
    const auto cbState=cbGap.available?marketFlowState(MarketFlowMetric::CoinbaseGap,cbGap.percent):MarketFlowState::Unavailable;
    const auto fundingState=matching?marketFlowFundingState(d,now):MarketFlowState::Unavailable;
    const auto state=[&](MarketFlowMetric metric,MarketFlowState value){auto* label=presentation(metric).state;label->setText(value==MarketFlowState::Unavailable?QString{}:marketFlowStateLabel(value));paintState(label,value);label->setAccessibleName(marketFlowMetricLabel(metric)+" · "+marketFlowStateLabel(value));label->setVisible(value!=MarketFlowState::Unavailable);};
    state(MarketFlowMetric::KoreaGap,gapState);state(MarketFlowMetric::Funding,fundingState);state(MarketFlowMetric::CoinbaseGap,cbState);
    ContractFlow flow;if(matching){flow.available=d.flowAvailable;flow.priceDirection=d.flowPriceDirection;flow.quantityDirection=d.flowQuantityDirection;}
    const auto flowLabel=marketFlowContractLabel(flow);oiPresentation_.state->setText(flowLabel);oiPresentation_.state->setAccessibleName(usabilityText(UsabilityText::FlowOiTitle)+" · "+flowLabel);oiPresentation_.state->setVisible(!flowLabel.isEmpty());
    summary_->setText(todaySentence(gapState,fundingState,cbState,calendarSummary_));summary_->setAccessibleName(summary_->text());fitSummary();
    QStringList derivativeStates;
    if(fundingState!=MarketFlowState::Unavailable)derivativeStates<<usabilityText(UsabilityText::FlowSummaryFunding).arg(marketFlowStateLabel(fundingState));
    if(cbState!=MarketFlowState::Unavailable)derivativeStates<<usabilityText(UsabilityText::FlowMetricCoinbaseGap)+" "+marketFlowStateLabel(cbState);
    if(!flowLabel.isEmpty())derivativeStates<<flowLabel;
    derivativesSummary_->setText(derivativeStates.join(QStringLiteral(" · ")));syncDerivativesHeader();
    const bool annualized=fundingState!=MarketFlowState::Unavailable;
    fundingPresentation_.meaning->setText(usabilityText(annualized?UsabilityText::FlowFundingMeaning:UsabilityText::FlowPublishedFundingMeaning));
    const auto comparison=marketFlowDayComparison(gap,previousGap_,now);
    const bool late=activeSinceMs_>0&&now-activeSinceMs_>=LateMs;
    const bool upbitMissing=upbitListing_==Listing::Missing,binanceMissing=binanceListing_==Listing::Missing;
    int legs=0;for(const auto& key:{"public:upbit:KRW-"+asset_,"public:binance:"+asset_+"USDT",QStringLiteral("public:upbit:KRW-USDT")})if(!quotes_.value(key).price.isEmpty())++legs;
    const auto gapStatus=upbitMissing?tr("업비트 원화마켓 미지원"):binanceMissing?usabilityText(UsabilityText::FlowGapNotOnBinance)
        :usabilityText(late?UsabilityText::FlowGapLate:UsabilityText::FlowGapProgress).arg(legs).arg(3);
    const auto cbStatus=binanceMissing?usabilityText(UsabilityText::FlowGapNotOnBinance):coinbaseMissing_?usabilityText(UsabilityText::FlowGapNotOnCoinbase):QString{};
    // Cell sub line: the published period rate and the next settlement time (public facts only).
    QString fundingLine;
    if(matching&&!d.fundingRate.isEmpty()&&d.fundingIntervalHours>0&&d.nextFundingTimeMs>now){
        const auto remaining=(d.nextFundingTimeMs-now)/60000;
        fundingLine=tr("%1시간마다 %2% · 다음 정산 %3시간 %4분 뒤").arg(d.fundingIntervalHours).arg(groupedAmount(ratePercent(d.fundingRate))).arg(remaining/60).arg(remaining%60);
    }
    ExploreMetricHistory gapHistory;
    if(previousGap_.available&&previousGap_.asset==asset_&&previousGap_.series.size()>=2){
        gapHistory.available=true;gapHistory.sourceKey="public:upbit-binance-gap:"+asset_;gapHistory.unit="%";qint64 previous=0;
        for(const auto& point:previousGap_.series){bool ok=false;const auto value=point.percent.toDouble(&ok);if(!ok||point.closeMs<=previous)continue;
            gapHistory.points.append({point.closeMs,value,previous>0&&point.closeMs-previous==MarketFlowPolicy::HourMs});previous=point.closeMs;}
    }
    const auto interval=matching && d.fundingIntervalHours > 0 ? tr("정산 주기 %1시간%2 · 다음 %3").arg(d.fundingIntervalHours)
        .arg(d.fundingIntervalDefault ? tr(" (공식 기본)") : QString{}).arg(timeText(d.nextFundingTimeMs)) : tr("정산 주기 · %1").arg(matching && !d.intervalReason.isEmpty() ? d.intervalReason : tr("조회 대기"));
    QStringList gapInfo{usabilityText(UsabilityText::FlowHistoryInfo),tr("업비트 KRW/자산 ÷ (Binance USDT/자산 × 업비트 KRW/USDT) − 1. 각 시세 180초 이내, 원천 시각 차이 30초 이내. % 소수 6자리에서 절삭. 시간에 따른 등락이 아닌 상대 가격차입니다.")};
    for(const auto& leg:gap.legs)if(!leg.baseAsset.isEmpty())gapInfo<<evidenceText(leg.quoteAsset+"/"+leg.baseAsset+" · "+leg.price,leg.evidence);
    QStringList cbInfo{tr("Coinbase USD/자산 ÷ (Binance USDT/자산 × Coinbase USD/USDT) − 1. 동일한 180초·시각 차이 30초 규칙. 환산 체결이 없으면 표시하지 않습니다. 시간에 따른 등락이 아닌 상대 가격차입니다.")};
    for(const auto& leg:cbGap.legs)if(!leg.baseAsset.isEmpty())cbInfo<<evidenceText(leg.quoteAsset+"/"+leg.baseAsset+" · "+leg.price,leg.evidence);
    QStringList fundingInfo{tr("공시 펀딩비율 · 변동 가능 · 확정 지급액 아님"),interval};
    fundingInfo<<evidenceText(tr("펀딩 비율"),d.fundingEvidence)<<evidenceText(tr("펀딩 주기 · 공식 기본값은 정상 조정 목록에서 누락된 계약만 적용"),d.intervalEvidence);
    if(matching&&!d.interestRate.isEmpty())fundingInfo<<tr("공시 interestRate: %1%").arg(ratePercent(d.interestRate));
    QStringList oiInfo{evidenceText(tr("미결제약정 원천 수량 · USD 명목금액 아님 · 계약 %1 · 기초자산 %2").arg(d.symbol,d.baseAsset),d.openInterestEvidence),
        tr("계약 흐름: 각 관측의 mark price·OI 시각 차이 30초 이내, 관측 간 120초 이내. 두 관측의 가격 방향 × 미결제약정 방향이며 롱·숏 유입 원인을 확정하지 않습니다.")};
    oiInfo<<(matching&&!d.flowLabel.isEmpty()?d.flowLabel+" · "+timeText(d.flowFromMs)+"~"+timeText(d.flowToMs):d.flowReason);
    const bool hasFunding=matching&&!d.fundingRate.isEmpty(),hasOi=matching&&!d.openInterest.isEmpty();
    // A read that answered without a value shows that, not an open-ended 불러오는 중.
    const auto missing=[&](const QString& reason){return matching&&!reason.isEmpty()&&reason!=MarketMetrics::pendingReason();};
    const auto pendingValue=[&](const QString& reason){return usabilityText(missing(reason)?UsabilityText::FlowNoData:UsabilityText::FlowChecking);};
    const auto set=[&](ExploreMetricCard* card,const QString& id,const QString& title,const QString& value,const QString& unit,const QString& fresh,const QStringList& info,const QList<ExploreMetricEvidence>& evidence){ExploreMetricCardData data;
        data.id=id;data.title=title;data.value=value;data.unit=unit;data.freshnessText=fresh;data.sourceKey="public:"+id+":"+asset_;data.evidence=evidence;data.publicInformation=info.join("\n\n")+tr("\n\n공개 시장 관측 · 매매 신호 아님\n실제 기록 배열 미제공 · 작은 차트 숨김");if(card==gap_)data.history=gapHistory;if(card==gap_&&!gap.available){data.publicInformation+="\n"+gapStatus;if(upbitMissing||binanceMissing)data.changeText=gapStatus;}if(card==coinbaseGap_&&!cbGap.available)data.changeText=cbStatus;if(card==funding_&&!fundingLine.isEmpty())data.changeText=fundingLine;if(card==funding_&&!hasFunding&&missing(d.fundingReason))data.changeText=d.fundingReason;if(card==oi_&&!hasOi&&missing(d.openInterestReason))data.changeText=d.openInterestReason;if(card==gap_&&comparison.available){data.changeText=usabilityText(UsabilityText::FlowDayChange).arg(groupedAmount(marketFlowRounded(comparison.differencePercentagePoints,2)));const auto& difference=comparison.differencePercentagePoints;data.direction=difference=="0"?MovementDirection::Flat:difference.startsWith('-')?MovementDirection::Down:MovementDirection::Up;}card->setData(data);card->show();};
    const auto evidenceFor=[&](const AssetEvidence& e,const QString& id,const QString& provider,const QString& state) {
        return ExploreMetricEvidence{id,provider,{},e.sourceTimeMs,e.receivedAtMs,e.complete,state};
    };
    const auto gapEvidence=[&](const ExchangePriceGap& priceGap,bool coinbase) {
        QList<ExploreMetricEvidence> list;
        for(int i=0;i<priceGap.legs.size();++i){const auto& leg=priceGap.legs[i];if(leg.evidence.receivedAtMs<=0)continue;
            const auto provider=i==1?QStringLiteral("Binance"):coinbase?QStringLiteral("Coinbase"):QStringLiteral("업비트");
            const auto& e=leg.evidence;const bool valid=e.complete&&e.sourceTimeMs>0&&e.sourceTimeMs<=now&&e.receivedAtMs>0&&e.receivedAtMs<=now&&now-e.sourceTimeMs<=MarketMetrics::FreshnessMs&&now-e.receivedAtMs<=MarketMetrics::FreshnessMs;
            list.append(evidenceFor(e,provider+":"+leg.quoteAsset+":"+leg.baseAsset,provider,valid?QStringLiteral("유효"):QStringLiteral("미확인")));}
        return list;
    };
    set(gap_,"upbit-binance-gap",usabilityText(UsabilityText::FlowKoreaGapTitle),gap.available?groupedAmount(marketFlowRounded(gap.percent,2)):usabilityText(upbitMissing||binanceMissing?UsabilityText::FlowGapUnavailable:UsabilityText::FlowChecking),gap.available?"%":"",gapFreshness(gap),gapInfo,gapEvidence(gap,false));
    set(coinbaseGap_,"coinbase-binance-gap",usabilityText(UsabilityText::FlowCoinbaseGapTitle),cbGap.available?groupedAmount(marketFlowRounded(cbGap.percent,2)):usabilityText(cbStatus.isEmpty()?UsabilityText::FlowChecking:UsabilityText::FlowGapUnavailable),cbGap.available?"%":"",gapFreshness(cbGap),cbInfo,gapEvidence(cbGap,true));
    set(funding_,"funding:binance",usabilityText(annualized?UsabilityText::FlowFundingTitle:UsabilityText::FlowPublishedFundingTitle),hasFunding?groupedAmount(annualized?marketFlowRounded(marketFlowAnnualFundingPercent(d.fundingRate,d.fundingIntervalHours),1):ratePercent(d.fundingRate)):pendingValue(d.fundingReason),hasFunding?"%":"",freshness(d.fundingEvidence),fundingInfo,matching&&d.fundingEvidence.receivedAtMs>0?QList<ExploreMetricEvidence>{evidenceFor(d.fundingEvidence,"funding:"+asset_,"Binance",hasFunding?"유효":"미확인"),evidenceFor(d.intervalEvidence,"interval:"+asset_,"Binance",d.fundingIntervalHours>0?"유효":"미확인")}:QList<ExploreMetricEvidence>{});
    set(oi_,"oi:binance",usabilityText(UsabilityText::FlowOiTitle),hasOi?groupedAmount(d.openInterest):pendingValue(d.openInterestReason),hasOi?d.openInterestUnit:QString{},freshness(d.openInterestEvidence),oiInfo,matching&&d.openInterestEvidence.receivedAtMs>0?QList<ExploreMetricEvidence>{evidenceFor(d.openInterestEvidence,"oi:"+asset_,"Binance",hasOi?"유효":"미확인")}:QList<ExploreMetricEvidence>{});
    refreshEvidence();
}
} // namespace keefi
