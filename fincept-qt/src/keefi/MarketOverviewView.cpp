#include "keefi/MarketOverviewView.h"
#include "keefi/MarketFlowPresentation.h"
#include <QGridLayout>
#include "keefi/ExploreMetricCard.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/InfoButton.h"
#include "keefi/UsabilityPresentation.h"
#include <QHBoxLayout>
#include <QComboBox>
#include <QDateTime>
#include <QHideEvent>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QShowEvent>
#include <QTimer>
#include <QTimeZone>
#include <QVBoxLayout>
namespace keefi {
namespace {
class MarketModeCombo final : public QComboBox {
  public:
    using QComboBox::QComboBox;
  protected:
    void paintEvent(QPaintEvent* event) override {
        QComboBox::paintEvent(event);QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(themeColors().text3,1.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        const QPointF center(width()-16,height()/2.0);QPolygonF arrow;
        arrow<<center+QPointF(-4,-2)<<center+QPointF(0,2)<<center+QPointF(4,-2);painter.drawPolyline(arrow);
    }
};
}
MarketOverviewView::MarketOverviewView(MarketOverview* overview,QWidget* parent):QWidget(parent),overview_(overview){
    setObjectName("marketOverviewView");auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(8);
    auto* title=new QLabel(tr("지금 볼 시장 맥락"),this);title->setObjectName("marketOverviewTitle");title->setTextFormat(Qt::PlainText);title->setProperty("type","sectionHeading");layout->addWidget(title);
    mode_=new MarketModeCombo(this);mode_->setObjectName("marketOverviewMode");mode_->setAccessibleName(tr("시장 시간대 모드"));mode_->setMinimumHeight(44);
    setThemeStyle(mode_,"QComboBox{background:${input};color:${text};border:1px solid ${field};border-radius:10px;padding:0 32px 0 12px;} QComboBox::drop-down{width:24px;border:0;background:transparent;}");
    for(const auto mode:{MarketTimeMode::Automatic,MarketTimeMode::Morning,MarketTimeMode::Korea,MarketTimeMode::Night})mode_->addItem(MarketOverview::modeLabel(mode),int(mode));
    layout->addWidget(mode_);
    // Market flow: one reference row under a rule, value on the right like a ledger line.
    flowLine_=new QWidget(this);flowLine_->setObjectName("marketFlowTreasuryLine");flowLine_->setAttribute(Qt::WA_StyledBackground,true);
    setThemeStyle(flowLine_,"QWidget#marketFlowTreasuryLine{border-top:1px solid ${rule};} QLabel{border:0;background:transparent;}");
    auto* line=new QGridLayout(flowLine_);line->setContentsMargins(0,12,0,0);line->setHorizontalSpacing(8);line->setVerticalSpacing(2);
    auto* flowName=new QLabel(usabilityText(UsabilityText::FlowTreasuryTitle),flowLine_);flowName->setObjectName("marketFlowTreasuryName");flowName->setFont(KeefiFonts::font(FontRole::Secondary14));setThemeStyle(flowName,"QLabel{color:${text2};}");line->addWidget(flowName,0,0);
    auto* flowInfo=new InfoButton(flowLine_);flowInfo->setInformation(usabilityText(UsabilityText::FlowTreasuryTitle),usabilityText(UsabilityText::FlowTreasuryInfo));line->addWidget(flowInfo,0,1,Qt::AlignLeft);
    flowValue_=new QLabel(flowLine_);flowValue_->setObjectName("marketFlowTreasuryValue");flowValue_->setFont(KeefiFonts::font(FontRole::Value22));flowValue_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);line->addWidget(flowValue_,0,2);
    flowDate_=new QLabel(flowLine_);flowDate_->setObjectName("marketFlowTreasuryDate");flowDate_->setTextFormat(Qt::RichText);line->addWidget(flowDate_,1,0,1,3);line->setColumnStretch(2,1);
    flowLine_->hide();layout->addWidget(flowLine_);
    treasury_=new ExploreMetricCard(this);treasury_->setObjectName("marketOverviewTreasuryCard");layout->addWidget(treasury_);evidence_=new ExploreEvidenceFooter(this);evidence_->setObjectName("marketOverviewEvidenceFooter");layout->addWidget(evidence_);layout->addStretch(1);
    connect(mode_,&QComboBox::currentIndexChanged,this,[this]{refresh();});if(overview_)connect(overview_,&MarketOverview::changed,this,&MarketOverviewView::refresh);
    auto* timer=new QTimer(this);timer->setInterval(1000);connect(timer,&QTimer::timeout,this,[this]{if(isVisible())refresh();});timer->start();refresh();
}
MarketOverviewView::~MarketOverviewView(){if(overview_){disconnect(overview_,nullptr,this,nullptr);overview_->setEnabled(false);}}
void MarketOverviewView::setMarketFlowMode(bool enabled){
    if(marketFlowMode_==enabled)return;marketFlowMode_=enabled;findChild<QLabel*>("marketOverviewTitle")->setVisible(!enabled);mode_->setVisible(!enabled);treasury_->setVisible(!enabled);flowLine_->setVisible(enabled);refresh();
}
void MarketOverviewView::setMode(MarketTimeMode mode){const auto index=mode_->findData(int(mode));if(index>=0)mode_->setCurrentIndex(index);}
MarketTimeMode MarketOverviewView::mode()const{return static_cast<MarketTimeMode>(mode_->currentData().toInt());}
void MarketOverviewView::showEvent(QShowEvent* e){QWidget::showEvent(e);if(overview_)overview_->setEnabled(true);refresh();}
void MarketOverviewView::hideEvent(QHideEvent* e){QWidget::hideEvent(e);if(overview_)overview_->setEnabled(backgroundActive_);}
void MarketOverviewView::setBackgroundActive(bool active){backgroundActive_=active;if(overview_)overview_->setEnabled(active||isVisible());}
bool MarketOverviewView::settled()const{
    if(!overview_)return true;if(!backgroundActive_&&!isVisible())return false;
    const auto data=overview_->treasury10Year();return !overview_->busy()&&(data.available||!data.reason.isEmpty());
}
void MarketOverviewView::setEvidenceStart(int start){start=qMax(1,start);if(evidenceStart_==start)return;evidenceStart_=start;refreshEvidence();}
int MarketOverviewView::evidenceCount()const{return evidence_?evidence_->evidenceCount():0;}
void MarketOverviewView::refreshEvidence(){
    if(!evidence_)return;const auto previous=evidence_->evidenceCount();evidence_->setEntries(treasury_->data().evidence,evidenceStart_);treasury_->setEvidenceNumbers(evidence_->numbers());
    if(previous!=evidence_->evidenceCount())emit evidenceChanged();
}
void MarketOverviewView::refresh(){
    const auto now=QDateTime::currentMSecsSinceEpoch();
    const auto data=overview_?overview_->treasury10Year(now):DailyTreasuryYield{};
    const auto date=data.observationDate.isValid()?data.observationDate.toString(Qt::ISODate):tr("미확인");
    const auto received=data.receivedAtMs>0?QDateTime::fromMSecsSinceEpoch(data.receivedAtMs,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm:ss")+" KST":tr("아직 수신하지 않음");
    ExploreMetricCardData card;card.id="treasury:10year";card.title=tr("미국 10년물 금리");
    card.value=data.available?data.percent:data.reason.isEmpty()?tr("공개 자료 연결 없음"):data.reason;card.unit=data.available?"%":QString{};
    card.sourceKey="public:treasury:10year";card.freshnessText=tr("일별 공시 · 미국 관측 %1 · 수신 %2").arg(date,received);
    card.publicInformation=tr("원천: 미국 재무부 · Daily Treasury Par Yield Curve Rates\n관측일: %1 (미국 현지 날짜)\n수신: %2\n%3\n국채의 장 마감 무렵 매수 호가에서 재무부가 산출한 일별 만기 10년 수익률(par yield)입니다. 시간대 모드는 관심 맥락을 바꾸며 휴장일·개장 여부를 판정하지 않습니다.\n\n%4\n\n실제 기록 배열 미제공 · 작은 차트 숨김")
        .arg(date,received,data.sourceUrl,MarketOverview::modeContext(mode(),now));
    if(data.receivedAtMs>0)card.evidence.append({"treasury:10year","미국 재무부",data.observationDate.isValid()?data.observationDate.toString(Qt::ISODate):QString{},0,data.receivedAtMs,data.available,data.available?"일별 공시":"미확인"});
    treasury_->setData(card);refreshEvidence();
    const auto shown=data.available?marketFlowRounded(data.percent,2):QString{};
    flowValue_->setFont(KeefiFonts::font(shown.isEmpty()?FontRole::Secondary14:FontRole::Value22));
    flowValue_->setText(shown.isEmpty()?usabilityText(UsabilityText::FlowChecking):shown+"%");
    flowValue_->setAccessibleName(usabilityText(UsabilityText::FlowTreasuryTitle)+" · "+flowValue_->text());flowValue_->setToolTip({});
    flowDate_->setText(data.observationDate.isValid()?KeefiFonts::mixedTextHtml(usabilityText(UsabilityText::FlowTreasuryDate).arg(data.observationDate.toString("MM/dd")),FontRole::Caption12):QString{});
    flowDate_->setVisible(data.observationDate.isValid());
}
} // namespace keefi
