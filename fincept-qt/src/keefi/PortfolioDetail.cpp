#include <QSignalBlocker>
#include "keefi/PortfolioDetail.h"
#include "keefi/BuildPolicy.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/ConnectionPresentation.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/TradingTypes.h"
#include "keefi/SolanaTransactionCodec.h"
#include "keefi/InfoButton.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/UsabilityPresentation.h"

#include <QApplication>
#include <QDateTime>
#include <QPainter>
#include <QRegularExpression>
#include <QTextLayout>
#include <QtMath>
#include <QUrl>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QStyle>
#include <QTimeZone>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace keefi {
namespace {
// Keep the original selectable QLabel text while shaping Korean/body and
// numeric/code runs with their distinct roles. No displayed value is recalculated.
class DetailLabel final : public QLabel {
  public:
    explicit DetailLabel(QWidget* parent):QLabel(parent){setMinimumWidth(0);}
    bool hasHeightForWidth()const override{return true;}
    int heightForWidth(int width)const override{return qCeil(shape(nullptr,qMax(1,width)));}
    QSize sizeHint()const override{return {qMin(400,qMax(1,QLabel::sizeHint().width())),heightForWidth(width()>0?width():400)};}
    QSize minimumSizeHint()const override{return {0,qCeil(QFontMetricsF(KeefiFonts::font(FontRole::Body15)).height())};}
  protected:
    void paintEvent(QPaintEvent*)override{QPainter painter(this);painter.setPen(palette().color(QPalette::WindowText));shape(&painter,width());}
  private:
    qreal shape(QPainter* painter,int available)const{
        const bool price=property("largePrice").toBool();
        const auto role=property("detailRole").isValid()?FontRole(property("detailRole").toInt()):FontRole::Body15;
        const auto body=KeefiFonts::font(role);
        static const QRegularExpression number(QStringLiteral("[+−-]?[0-9][0-9,.:/%+−-]*"));
        static const QRegularExpression code(QStringLiteral("(?<![A-Za-z0-9_])[A-Z][A-Z0-9_./-]*(?![A-Za-z0-9_])"));
        qreal y=0;
        for(const auto& paragraph:text().split('\n')){
            QTextLayout layout(paragraph,body);QList<QTextLayout::FormatRange> formats;
            for(const auto& entry:QList<QPair<QRegularExpression,FontRole>>{{number,price?FontRole::Price44:FontRole::Number11},{code,FontRole::Number11}}){
                auto matches=entry.first.globalMatch(paragraph);while(matches.hasNext()){const auto m=matches.next();QTextLayout::FormatRange run;run.start=m.capturedStart();run.length=m.capturedLength();run.format.setFont(KeefiFonts::font(entry.second));formats.append(run);}}
            layout.setFormats(formats);QTextOption option;option.setAlignment(alignment()&Qt::AlignHorizontal_Mask);option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout.setTextOption(option);layout.beginLayout();
            bool any=false;while(true){auto line=layout.createLine();if(!line.isValid())break;line.setLineWidth(qMax(1,available));line.setPosition({0,y});y+=line.height();any=true;}layout.endLayout();
            if(painter)layout.draw(painter,{0,0});if(!any)y+=QFontMetricsF(body).height();
        }return y;
    }
};
QLabel* plainLabel(const QString& name, QWidget* parent) {
    auto* label = new DetailLabel(parent);
    label->setObjectName(name);label->setTextFormat(Qt::PlainText);label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return label;
}
QString evidenceProvider(const AssetEvidence& evidence) {
    // Only known provider names leave the provenance input. Account scope, URL
    // paths/queries, aliases and arbitrary source suffixes never reach the footer.
    const auto source=evidence.source.trimmed();const auto host=QUrl(evidence.url).host().toLower();
    if(host=="api.upbit.com"||source.startsWith("업비트")||source.startsWith("upbit",Qt::CaseInsensitive))return QStringLiteral("업비트");
    if(host=="api.bithumb.com"||source.startsWith("빗썸")||source.startsWith("bithumb",Qt::CaseInsensitive))return QStringLiteral("빗썸");
    if(host=="api.binance.com"||host=="fapi.binance.com"||host=="stream.binance.com"||source.startsWith("Binance",Qt::CaseInsensitive))return QStringLiteral("Binance");
    if(host=="www.okx.com"||source.startsWith("OKX",Qt::CaseInsensitive))return QStringLiteral("OKX");
    if(host=="openapi.tossinvest.com"||source.startsWith("토스")||source.startsWith("toss",Qt::CaseInsensitive))return QStringLiteral("토스증권");
    if(host=="api.kiwoom.com"||source.startsWith("키움")||source.startsWith("kiwoom",Qt::CaseInsensitive))return QStringLiteral("키움증권");
    if(host=="openapi.ls-sec.co.kr"||source.startsWith("LS",Qt::CaseInsensitive))return QStringLiteral("LS증권");
    if(host=="openapi.koreainvestment.com"||source.startsWith("한국투자")||source.startsWith("kis",Qt::CaseInsensitive))return QStringLiteral("한국투자증권");
    if(host=="api.exchange.coinbase.com"||source.startsWith("Coinbase",Qt::CaseInsensitive))return QStringLiteral("Coinbase");
    if(host=="api.mainnet-beta.solana.com"||source.startsWith("Solana",Qt::CaseInsensitive)||source.startsWith("solana",Qt::CaseInsensitive))return QStringLiteral("Solana");
    return QStringLiteral("원천 미제공");
}
ExploreMetricEvidence viewEvidence(const AssetEvidence& evidence,const QString& state){
    return {evidence.source+QChar(0)+evidence.url+QChar(0)+evidence.scope,evidenceProvider(evidence),{},evidence.sourceTimeMs,evidence.receivedAtMs,evidence.complete,state};
}
QString kst(qint64 ms) {
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).toTimeZone(QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm:ss") + QObject::tr(" KST")
                  : QObject::tr("제공되지 않음");
}
bool contractCount(const PortfolioHoldingView& h) {
    return h.kind == AssetComponentKind::PositionNotional && h.provider == "okx" &&
        (h.market == "okx_unified" || h.market == "okx_futures");
}
QString identityNote(const QString& identity) {
    if (identity.startsWith("cash:")) return QObject::tr("원화 현금 · 연결 계좌 합산");
    if (identity.startsWith("exchange:")) return QObject::tr("거래소 코인 · 같은 티커 기준으로 보관처를 묶어 표시합니다. 평가도 같은 티커의 원화 가격을 사용합니다.");
    if (identity.startsWith("stock:KR:")) return QObject::tr("국내주식 · 종목코드 기준");
    if (identity == "solana:native") return QObject::tr("Solana 기본 자산(SOL)");
    return QObject::tr("Solana 토큰 · Mint Address 기준");
}
QString valueLine(const PortfolioHoldingView& h) {
    switch (h.state) {
    case PortfolioValueState::Fresh:
        return h.asOfTimeMs > 0
            ? QObject::tr("원화 평가  %1 KRW · 마지막 확인 평가 · %2 · 저장 당시 합계 포함").arg(groupedAmount(h.value), kst(h.asOfTimeMs))
            : QObject::tr("원화 평가  %1 KRW · 최신 평가 · 합계 포함").arg(groupedAmount(h.value));
    case PortfolioValueState::Reference: return QObject::tr("원화 평가  참고 %1 KRW · 합계 제외 · %2").arg(groupedAmount(h.referenceValue), h.reason);
    case PortfolioValueState::NotCounted: return QObject::tr("원화 평가  합계 미포함 · %1").arg(h.reason);
    case PortfolioValueState::Unvalued: break;
    }
    return QObject::tr("원화 평가  — · 미평가 · %1").arg(h.reason.isEmpty() ? QObject::tr("사유 미확인") : h.reason);
}
QString shortValue(const PortfolioHoldingView& h, const PortfolioMoneyDisplay& display = {}) {
    switch (h.state) {
    case PortfolioValueState::Fresh: return display.amount(h.value) + " " + display.unit();
    case PortfolioValueState::Reference: return QObject::tr("참고 %1 %2").arg(display.amount(h.referenceValue), display.unit());
    case PortfolioValueState::NotCounted: return QObject::tr("합계 미포함");
    case PortfolioValueState::Unvalued: break;
    }
    return QObject::tr("미평가");
}
QPair<QString, QString> placeRow(const PortfolioHoldingView& h, const PortfolioMoneyDisplay& display = {}) {
    const auto quantity = contractCount(h) ? portfolioHoldingQuantity(h) :
        h.quantity.isEmpty() ? QObject::tr("수량 확인 필요") : groupedAmount(h.quantity) + " " + portfolioAssetLabel(h.asset,h.displaySymbol);
    // Cash in KRW is its own valuation; say the amount once.
    QString value = h.asset == QLatin1String("KRW") && h.state == PortfolioValueState::Fresh && display.unit() == QLatin1String("KRW")
        ? shortValue(h, display) : quantity + '\n' + shortValue(h, display);
    if (!h.providerPnl.isEmpty()) value += QObject::tr("\n업체 평가손익 %1").arg(providerPnlText(h));
    return {h.accountAlias + " · " + connectionMarketName(h.market),
            value};
}
QString holdingBlock(const PortfolioHoldingView& h) {
    const auto quantity = contractCount(h) ? portfolioHoldingQuantity(h) :
        QObject::tr("수량  %1 %2").arg(h.quantity.isEmpty() ? QObject::tr("확인 필요") : groupedAmount(h.quantity), portfolioAssetLabel(h.asset,h.displaySymbol));
    QStringList lines{h.accountAlias + " · " + connectionMarketName(h.market), quantity +
            (h.market == "spot" ? QObject::tr(" · 사용 가능과 주문 묶임 수량의 합계") : QString{}),
        valueLine(h),
        QObject::tr("잔고 기준  %1").arg(kst(h.balanceTimeMs))};
    if (h.state == PortfolioValueState::Fresh || h.state == PortfolioValueState::Reference)
        lines.append(QObject::tr("가격 근거  %1 · 원본 %2 · 수신 %3").arg(h.priceEvidence.source.isEmpty() ? QObject::tr("출처 미확인") : h.priceEvidence.source,
            kst(h.priceEvidence.sourceTimeMs), kst(h.priceEvidence.receivedAtMs)));
    for(const auto& leg:h.conversionEvidence)
        lines.append(QObject::tr("환산 근거  %1 · 원본 %2 · 수신 %3\n%4").arg(leg.source,kst(leg.sourceTimeMs),kst(leg.receivedAtMs),leg.url));
    if(h.asset.startsWith("solana:")){
        lines.append("Mint Address  "+h.asset.mid(7));
        lines.append(h.metadataEvidence.complete?"메타데이터  "+h.metadataEvidence.source:"메타데이터  "+h.metadataIssue);
        if(!h.priceIssue.isEmpty())lines.append("가격 조회  "+h.priceIssue);
        else if(h.state==PortfolioValueState::Unvalued)lines.append("가격 조회와 별도로 원화 환산 근거를 확인하지 못했습니다.");
    }
    if (h.presence == AssetPresence::NeedsCheck) lines.append(QObject::tr("수량 확인 필요 · 잔고 0으로 간주하지 않습니다."));
    if (!h.providerPnl.isEmpty()) lines.append(QObject::tr("업체 제공 평가손익  %1 %2\n손익 출처  %3 · 수신 %4")
        .arg(groupedAmount(h.providerPnl), h.providerPnlCurrency, h.providerPnlSource, kst(h.providerPnlTimeMs)));
    return lines.join('\n');
}
} // namespace

ExploreMetricEvidence publicMarketQuoteEvidence(const PublicMarketRow& row,qint64 nowMs) {
    const bool timeKnown=nowMs>0&&row.sourceTimeMs>0&&row.receivedAtMs>0&&
        row.sourceTimeMs<=nowMs&&row.receivedAtMs<=nowMs;
    const auto state=!row.priced||!timeKnown?QStringLiteral("미확인"):
        nowMs-row.sourceTimeMs>60000?QStringLiteral("지연"):QStringLiteral("유효");
    return {QStringLiteral("coin:upbit:")+row.market,QStringLiteral("업비트"),{},
        row.sourceTimeMs,row.receivedAtMs,row.priced,state};
}

AssetDetailPanel::AssetDetailPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("assetDetail");
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("card", false);
    setThemeStyle(this,"QWidget#assetDetail { background: ${bg}; border: 0; border-top: 1px solid ${rule}; border-radius: 0; }");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 14, 14, 14);
    back_ = new QPushButton(tr("‹ 목록"), this);
    back_->setObjectName("assetDetailBack");
    back_->setAccessibleName(tr("자산 목록으로 돌아가기"));
    back_->setProperty("compact", true);
    back_->hide();
    outer->addWidget(back_, 0, Qt::AlignLeft);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("assetDetailScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // Keep the detail readable when status/header rows grow at the smallest
    // window size. The enclosing portfolio scroll area handles the overflow.
    scroll->setMinimumHeight(240);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    auto* content = new QWidget(scroll);
    content->setObjectName("assetDetailContent");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 4, 0);
    layout->setSpacing(10);
    layout->setAlignment(Qt::AlignTop);
    empty_ = plainLabel("assetDetailEmpty", content);
    empty_->setText(tr("자산을 선택하세요."));
    empty_->setProperty("type", "muted");
    // Ledger head keeps the existing name and identity as text.
    auto* head = new QHBoxLayout;
    head->setSpacing(12);
    auto* names = new QVBoxLayout;
    names->setSpacing(2);
    title_ = plainLabel("assetDetailTitle", content);
    title_->setProperty("type", "section");title_->setProperty("detailRole",int(FontRole::Title17));
    subtitle_ = plainLabel("assetDetailIdentity", content);
    subtitle_->setProperty("type", "muted");subtitle_->setProperty("detailRole",int(FontRole::Caption12));
    names->addWidget(title_);
    names->addWidget(subtitle_);
    head->addLayout(names, 1);
    // The one figure that matters, then where it sits; the rest is evidence.
    headline_ = plainLabel("assetDetailHeadline", content);
    headline_->setProperty("type", "hero");headline_->setProperty("detailRole",int(FontRole::Title17));
    setThemeStyle(headline_,"QLabel { font-family: ${bodyFamily}; font-size: ${Title17Px}px; font-weight: 700; }");
    references_=new QLabel(content);references_->setObjectName("assetDetailReferences");references_->setTextFormat(Qt::PlainText);references_->setFont(KeefiFonts::font(FontRole::Number11));
    setThemeStyle(references_,"QLabel { color: ${accentText}; }");references_->hide();
    headlineSub_ = plainLabel("assetDetailHeadlineSub", content);
    totals_ = plainLabel("assetDetailTotals", content);
    totals_->hide(); // Kept as the full text form of the figure for accessibility and records.
    places_ = new QWidget(content);
    places_->setObjectName("assetDetailPlaces");
    placesLayout_ = new QVBoxLayout(places_);
    placesLayout_->setContentsMargins(0, 4, 0, 4);
    placesLayout_->setSpacing(6);
    connections_ = new QPushButton(tr("계좌·연결에서 상태 보기"), content);
    connections_->setObjectName("assetDetailConnections");
    evidence_ = new QPushButton(content);
    evidence_->setObjectName("assetDetailEvidence");
    evidence_->setCheckable(true);
    evidence_->setProperty("disclose", true);
    setThemeStyle(evidence_,"QPushButton { border:0; border-radius:0; background:transparent; color:${text2}; text-decoration:underline; padding:0; text-align:left; }");
    evidence_->setAccessibleName(tr("자산 상세 정보 펼치기"));
    blocks_ = new QWidget(content);
    blocks_->setObjectName("assetDetailHoldings");
    blockLayout_ = new QVBoxLayout(blocks_);
    blockLayout_->setContentsMargins(0, 4, 0, 0);
    blockLayout_->setSpacing(10);
    layout->addWidget(empty_);
    layout->addLayout(head);
    auto* headlineRow=new QHBoxLayout;headlineRow->setContentsMargins(0,0,0,0);headlineRow->setSpacing(8);headlineRow->addWidget(headline_,1);headlineRow->addWidget(references_,0,Qt::AlignBottom);layout->addLayout(headlineRow);
    for (auto* widget : {static_cast<QWidget*>(headlineSub_), static_cast<QWidget*>(totals_), places_,
                         static_cast<QWidget*>(evidence_), blocks_, static_cast<QWidget*>(connections_)})
        layout->addWidget(widget);
    valuationExclusion_ = new QPushButton(content);
    valuationExclusion_->setObjectName("assetValuationExclusion");
    valuationExclusion_->setToolTip(tr("보유 수량은 유지하고 이 Mint를 평가 합계에서 제외하거나 다시 포함합니다."));
    valuationExclusion_->hide();
    layout->addWidget(valuationExclusion_);
    connect(valuationExclusion_, &QPushButton::clicked, this, [this] {
        if (!selectedValuationAsset_.isEmpty()) emit valuationExclusionRequested(selectedValuationAsset_, !valuationExcluded_);
    });
    layout->addStretch();
    evidenceFooter_=new ExploreEvidenceFooter(content);evidenceFooter_->setObjectName("assetDetailEvidenceFooter");layout->addWidget(evidenceFooter_);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    connect(back_, &QPushButton::clicked, this, &AssetDetailPanel::backRequested);
    connect(connections_, &QPushButton::clicked, this, &AssetDetailPanel::connectionsRequested);
    connect(evidence_, &QPushButton::toggled, this, &AssetDetailPanel::applyEvidenceFold);
    clearDetail();
}

void AssetDetailPanel::applyEvidenceFold() {
    // Evidence stays available internally, but this surface no longer exposes
    // a disclosure. Programmatic toggles must not restore the retired body.
    evidence_->setText({});evidence_->setToolTip({});evidence_->setAccessibleName({});
    evidence_->hide();blocks_->hide();
}

void AssetDetailPanel::setBody(const QString& title, const QString& subtitle, const QString& headline, const QString& headlineSub,
                               const QString& subTone, const QString& totals, const QList<QPair<QString, QString>>& places,
                               const QStringList& blocks, BodyMode mode) {
    setReferenceNumbers({});evidenceFooter_->setEntries({});
    assetShareIdentity_.clear();if(assetShare_)assetShare_->hide();
    publicPresentation_=false;publicFacts_=mode==BodyMode::PublicFacts;
    // A refresh of the same public selection keeps its card, chart link and indicators; the calls that
    // follow set their final state. Hiding them first left a frame laid out without them.
    if(!preservePublicInformation_){
        chartIdentity_.clear();if(chartLink_)chartLink_->hide();
        if(publicCard_){publicCard_->clearPrivate();privateLinkageAllowed_=false;publicCard_->clear();publicCard_->hide();}setPublicIndicators({});
    }
    valuationExclusion_->hide(); selectedValuationAsset_.clear();
    while (auto* item = blockLayout_->takeAt(0)) { delete item->widget(); delete item; }
    // Rows are reused: a refresh changes their text, not the widgets.
    while (placesLayout_->count() > places.size()) { auto* item = placesLayout_->takeAt(placesLayout_->count() - 1); delete item->widget(); delete item; }
    title_->setText(title);
    subtitle_->setText(subtitle);
    totals_->setText(totals);
    headline_->setText(headline);
    headlineSub_->setText(headlineSub);
    const bool quiet = headline == tr("미평가") || headline == tr("참고 평가") || headline == tr("합계 제외") ||
        headline == tr("확인 필요") || headline == tr("조회 중") || headline == tr("현재가 미확인");
    headline_->setProperty("quiet", quiet);
    headline_->setProperty("largePrice", !quiet && QRegularExpression("[0-9]").match(headline).hasMatch());
    headline_->updateGeometry();headline_->update();
    headline_->style()->unpolish(headline_);
    headline_->style()->polish(headline_);
    headlineSub_->setProperty("tone", subTone);
    headlineSub_->style()->unpolish(headlineSub_);
    headlineSub_->style()->polish(headlineSub_);
    for (int i = 0; i < places.size(); ++i) {
        auto* row = i < placesLayout_->count() ? placesLayout_->itemAt(i)->widget() : nullptr;
        if (!row) {
            row = new QWidget(places_);
            row->setObjectName(QStringLiteral("assetDetailPlace%1").arg(i));
            row->setProperty("placeRow", true);
            row->setAttribute(Qt::WA_StyledBackground, true);
            setThemeStyle(row,"QWidget[placeRow=\"true\"] { background: ${bg}; border: 0; border-top: 1px solid ${rule}; border-radius: 0; }");
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(12, 8, 12, 8);
            auto* left = plainLabel(QStringLiteral("assetDetailPlaceName%1").arg(i), row);
            auto* right = plainLabel(QStringLiteral("assetDetailPlaceValue%1").arg(i), row);
            right->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rowLayout->addWidget(left, 1);
            rowLayout->addWidget(right, 1);
            placesLayout_->addWidget(row);
            row->show(); // Rows follow the container: hidden only while places_ is hidden.
        }
        row->findChild<QLabel*>(QStringLiteral("assetDetailPlaceName%1").arg(i))->setText(places[i].first);
        row->findChild<QLabel*>(QStringLiteral("assetDetailPlaceValue%1").arg(i))->setText(places[i].second);
    }
    for (int i = 0; i < blocks.size(); ++i) {
        auto* block = plainLabel(QStringLiteral("assetDetailHolding%1").arg(i), blocks_);
        block->setText(blocks[i]);
        block->setProperty("detailBlock", true);
        blockLayout_->addWidget(block);
    }
    const bool any = !title.isEmpty();
    // Qt lays out a shown widget at once but a hidden one only later, so the ledger head is
    // never shown for a moment under a public card: that frame pushed the card down.
    const bool ledger = any && mode == BodyMode::Ledger;
    empty_->setVisible(!any);
    for (auto* widget : {static_cast<QWidget*>(title_), static_cast<QWidget*>(connections_),
                         static_cast<QWidget*>(evidence_)})
        widget->setVisible(ledger);
    subtitle_->setVisible(ledger && !subtitle.isEmpty());
    headline_->setVisible(ledger && !headline.isEmpty());
    headlineSub_->setVisible(ledger && !headlineSub.isEmpty());
    places_->setVisible(any && !places.isEmpty() && mode != BodyMode::PublicCard);
    applyEvidenceFold();
}
void AssetDetailPanel::settleLayout() {
    // Final geometry before returning to the event loop: no frame shows a half-applied refresh.
    if (auto* layout = title_->parentWidget()->layout()) layout->activate();
}

void AssetDetailPanel::enableAssetShareControl() {
    if(assetShare_)return;
    auto* content=findChild<QWidget*>("assetDetailContent");
    auto* layout=content?qobject_cast<QVBoxLayout*>(content->layout()):nullptr;if(!layout)return;
    assetShare_=new QPushButton(tr("자산 카드"),content);assetShare_->setObjectName("assetDetailShare");assetShare_->setMinimumSize(44,44);
    assetShare_->setToolTip(tr("선택한 자산의 공개 가격 변화를 카드로 확인해요. 보유 금액·수량은 기본 숨김입니다."));
    layout->insertWidget(3,assetShare_);assetShare_->setVisible(selected_&&!publicPresentation_&&!assetShareIdentity_.isEmpty());
    connect(assetShare_,&QPushButton::clicked,this,[this]{
        if(selected_&&!publicPresentation_&&!assetShareIdentity_.isEmpty())emit assetShareRequested(assetShareIdentity_);
    });
}

void AssetDetailPanel::showGroup(const PortfolioGroupView& group, const QString& allocationTotal, const PortfolioMoneyDisplay& display, bool showUnvalued) {
    selected_ = true;
    QStringList blocks;
    QList<QPair<QString, QString>> places;
    const auto quantity = portfolioQuantity(group);
    if (group.holdings.size() > 1 && !quantity.isEmpty()) places.append({tr("총 보유"), groupedAmount(quantity) + ' ' + portfolioAssetLabel(group.asset,group.displaySymbol)});
    if (!allocationTotal.isEmpty() && decimal::positive(portfolioAllocationValue(group)))
        places.append({tr("전체 비중"), portfolioShareText(portfolioAllocationValue(group), allocationTotal)});
    for (const auto& h : group.holdings) {
        if(!showUnvalued && h.state==PortfolioValueState::Unvalued)continue;
        blocks.append(holdingBlock(h)); places.append(placeRow(h, display));
    }
    QStringList flags;
    if (showUnvalued && group.unvalued) flags.append(tr("미평가 %1건").arg(group.unvalued));
    if (group.references) flags.append(tr("참고 평가 %1건 · 합계 제외").arg(group.references));
    if (group.notCounted) flags.append(tr("합계 미포함 %1건").arg(group.notCounted));
    const bool anyFresh = std::any_of(group.holdings.cbegin(), group.holdings.cend(), [](const auto& h) { return h.state == PortfolioValueState::Fresh; });
    const qint64 asOf = group.holdings.isEmpty() ? 0 : group.holdings.first().asOfTimeMs;
    const bool recorded = asOf > 0 && std::all_of(group.holdings.cbegin(), group.holdings.cend(), [asOf](const auto& h) { return h.asOfTimeMs == asOf; });
    const auto totals = (recorded ? tr("보관처 %1곳 · 마지막 확인 평가 합계  %2") : tr("보관처 %1곳 · 최신 평가 합계  %2"))
        .arg(group.storages.size()).arg(anyFresh && group.freshTotal.isEmpty() ? tr("자료 부족")
            : anyFresh ? krwAmount(group.freshTotal) + tr(" KRW") : tr("— (평가 없음)")) +
        (recorded ? "\n" + tr("마지막 확인 · %1").arg(kst(asOf)) : QString{}) +
        (flags.isEmpty() ? QString{} : "\n" + flags.join(" · "));
    const auto identity = identityNote(group.identity) +
        (group.identity.startsWith("exchange:") || group.identity.startsWith("stock:") || group.identity == "solana:native" ? QString{} : "\n" + group.identity);
    blocks.append(identity);
    const auto headline = group.excluded ? tr("평가 대상 제외") : anyFresh && group.freshTotal.isEmpty() ? tr("자료 부족") : anyFresh ? display.amount(group.freshTotal) + " " + display.unit() :
        group.unvalued ? tr("미평가") : group.references ? tr("참고 평가") : tr("합계 제외");
    const bool partial = anyFresh && (group.unvalued || group.references);
    auto headlineSub = (recorded ? tr("마지막 확인 · %1").arg(kst(asOf)) : QString{}) +
        (partial ? (recorded ? "\n" : "") + tr("일부 평가 제외") : QString{});
    if (!display.status().isEmpty()) headlineSub += (headlineSub.isEmpty() ? QString{} : "\n") + display.status();
    setBody(portfolioGroupLabel(group), group.displayName, headline, headlineSub,
            partial ? QStringLiteral("warning") : QString{}, totals, places, blocks);
    QList<ExploreMetricEvidence> proof;
    for(const auto& holding:group.holdings)if(holding.state==PortfolioValueState::Fresh||holding.state==PortfolioValueState::Reference){
        if(anyFresh&&holding.state!=PortfolioValueState::Fresh)continue;
        proof.append(viewEvidence(holding.priceEvidence,holding.state==PortfolioValueState::Reference?QStringLiteral("지연"):holding.priceEvidence.complete?QStringLiteral("유효"):QStringLiteral("불완전")));
        for(const auto& leg:holding.conversionEvidence)proof.append(viewEvidence(leg,holding.state==PortfolioValueState::Reference?QStringLiteral("지연"):leg.complete?QStringLiteral("유효"):QStringLiteral("불완전")));
    }
    evidenceFooter_->setEntries(proof);setReferenceNumbers(evidenceFooter_->numbers());
    const bool mint = group.asset.startsWith("solana:") && SolanaTransactionCodec::publicKeyValid(group.asset.mid(7));
    if (mint && (group.unvalued || group.excluded)) {
        selectedValuationAsset_ = group.asset; valuationExcluded_ = group.excluded > 0;
        valuationExclusion_->setText(valuationExcluded_ ? tr("평가 대상에 포함") : tr("평가 대상 제외"));
        valuationExclusion_->show();
    }
    connections_->setText(tr("계좌·연결"));
    connections_->setVisible(group.unvalued || group.references || group.presence == AssetPresence::NeedsCheck);
    assetShareIdentity_=group.identity;if(assetShare_)assetShare_->setVisible(!assetShareIdentity_.isEmpty());
}

void AssetDetailPanel::showMarket(const PublicMarketRow& row, bool personalReadable, const PortfolioGroupView* holdings, bool coverageIncomplete) {
    selected_ = true;
    const auto now=QDateTime::currentMSecsSinceEpoch();
    QStringList market;
    if (row.priced) {
        const auto change = changePercentText(row.signedChangeRate);
        market << tr("현재가  %1 KRW").arg(priceAmount(row.price))
               << tr("전일 대비  %1 · 전일 종가 %2 KRW (UTC 0시 = KST 09시 기준)").arg(change.isEmpty() ? tr("확인 불가") : change, priceAmount(row.previousClose))
               << tr("시세 반영 시각  %1 · 수신 %2").arg(kst(row.sourceTimeMs), kst(row.receivedAtMs));
        const auto age = now - row.sourceTimeMs;
        if (row.sourceTimeMs > 0 && age > 60000) market << tr("반영 시각이 %1분 전입니다. 새로고침으로 다시 확인하세요.").arg(age / 60000);
    } else market << tr("현재가  — · 아직 조회하지 않았거나 조회에 실패했습니다.");
    if (row.warning) market << tr("업비트 투자 유의 종목입니다.");
    if (row.caution) market << tr("업비트 주의 경보가 있습니다.");
    market << tr("출처  업비트 공식 공개 API · 계좌·인증 없이 조회");
    QStringList blocks{market.join('\n')};
    QString personal;
    if (!personalReadable) personal = tr("내 보유 현황은 잠금 해제 후 확인할 수 있습니다.");
    else if (holdings) personal = tr("내 보유 · 같은 티커(%1) 기준 연결 · 보관처 %2곳").arg(row.base).arg(holdings->storages.size());
    else personal = coverageIncomplete ? tr("내 보유 · 확인되지 않은 계좌가 있어 보유 여부를 확정할 수 없습니다.")
                                       : tr("내 보유 · 연결된 계좌에서 %1 보유가 확인되지 않았습니다.").arg(row.base);
    blocks.append(personal);
    QList<QPair<QString, QString>> places;
    if (personalReadable && holdings) {
        for (const auto& h : holdings->holdings) { blocks.append(holdingBlock(h)); places.append(placeRow(h)); }
        if (std::any_of(holdings->holdings.cbegin(), holdings->holdings.cend(), [](const auto& h) { return h.state != PortfolioValueState::Fresh; }))
            places.append({tr("보유 평가"), tr("시세와 별도로 갱신")});
    }
    else places.append({tr("내 보유"), personal.section(QStringLiteral(" · "), -1)});
    const auto change = changePercentText(row.signedChangeRate);
    const auto headline = row.priced ? tr("%1 KRW · 전일 대비 %2").arg(priceAmount(row.price), change) : tr("현재가 미확인");
    QString tone;
    if (row.priced && row.signedChangeRate.startsWith('-')) tone = QStringLiteral("down");
    else if (row.priced && !change.isEmpty() && !change.startsWith("0.00") && !change.startsWith("+0.00")) tone = QStringLiteral("up");
    preservePublicInformation_=publicCard_&&publicPresentation_&&publicCard_->data().id==row.market&&publicCard_->data().sourceKey=="coin:upbit:"+row.market;
    setBody(row.koreanName.isEmpty() ? row.base : row.koreanName,
        tr("업비트 KRW 시장 · %1%2").arg(row.market, row.englishName.isEmpty() ? QString{} : " · " + row.englishName),
        row.priced ? priceAmount(row.price) + tr(" KRW") : tr("현재가 미확인"),
        row.priced ? (tone == "up" ? tr("▲ ") : tone == "down" ? tr("▼ ") : QString{}) + tr("전일 대비 %1 · 오늘 09:00 기준").arg(change.isEmpty() ? tr("확인 불가") : change) : QString{},
        tone, headline, places, blocks, publicCard_ ? BodyMode::PublicCard : BodyMode::Ledger);
    preservePublicInformation_=false;
    if(publicCard_){
        ExploreMetricCardData data;data.id=row.market;data.title=title_->text();data.value=row.priced?priceAmount(row.price):tr("현재가 미확인");data.unit="KRW";
        data.changeText=row.priced?tr("전일 대비 %1 · 오늘 09:00 기준").arg(change):QString{};data.direction=tone=="up"?MovementDirection::Up:tone=="down"?MovementDirection::Down:MovementDirection::Flat;
        data.sourceKey="coin:upbit:"+row.market;data.freshnessText=tr("원천 %1 · 수신 %2").arg(kst(row.sourceTimeMs),kst(row.receivedAtMs));data.publicInformation=market.join('\n');
        QStringList linkage{personal};for(const auto& place:places)linkage.append(place.first+"\n"+place.second);
        showPublicCard(data);setPrivateLinkage(personalReadable,linkage.join("\n\n"));
    }
    if(row.priced)setPublicEvidence({publicMarketQuoteEvidence(row,now)});
}

void AssetDetailPanel::showQuote(const QString& name, const QString& code, const QString& price, const QString& changePercent,
                                 const QList<QPair<QString, QString>>& rows, const QStringList& evidence, const QString& changeBasis,const QString& currency,const QString& sourceKey,bool previousQuote) {
    selected_ = true;
    bool ok = false;
    const double change = changePercent.toDouble(&ok);
    const QString tone = !ok || std::abs(change) < 0.005 ? QString{} : change > 0 ? QStringLiteral("up") : QStringLiteral("down");
    const auto basis = changeBasis.isEmpty() ? tr("전일 대비") : changeBasis;
    const QString sub = !ok ? basis + tr(" 미확인")
        : (tone == "up" ? tr("▲ +") : tone == "down" ? tr("▼ ") : QString{}) + QString::number(change, 'f', 2) + "% · " + basis;
    preservePublicInformation_=publicCard_&&publicPresentation_&&publicCard_->data().id==code.section(' ',0,0)&&publicCard_->data().sourceKey==sourceKey;
    setBody(name, code, price, sub, tone, price + " · " + sub, rows, {evidence.join('\n')}, publicCard_ ? BodyMode::PublicFacts : BodyMode::Ledger);preservePublicInformation_=false;publicFacts_=true;
    connections_->hide(); // Public quotes have no connection to inspect.
    if(publicCard_){ExploreMetricCardData data;data.id=code.section(' ',0,0);data.title=name;data.value=price;data.unit=currency;data.sourceKey=sourceKey;
        if(currency=="USD"&&data.value.startsWith('$'))data.value.remove(0,1);
        if(!currency.isEmpty()&&data.value.endsWith(" "+currency))data.value.chop(currency.size()+1);
        data.changeText=!ok?basis+tr(" 미확인"):(change>0?"+":QString{})+QString::number(change,'f',2)+"% · "+basis;
        data.direction=!ok?MovementDirection::Unknown:tone=="up"?MovementDirection::Up:tone=="down"?MovementDirection::Down:MovementDirection::Flat;
        data.publicInformation=evidence.join('\n');data.freshnessText=(previousQuote?tr("이전 시세 · "):QString{})+evidence.value(0);if(previousQuote)data.publicInformation+=tr("\n이전 수신 시세입니다. 원래 수신 시각을 유지하며 현재 시세로 간주하지 않습니다.");showPublicCard(data);}
}

void AssetDetailPanel::enableExplorationControls() {
    if (favourite_) return;
    auto* content = title_->parentWidget();
    auto* layout = qobject_cast<QVBoxLayout*>(content->layout());
    publicCard_=new ExploreMetricCard(content);publicCard_->setObjectName("assetDetailMetric");publicCard_->setPresentation(ExploreMetricCard::Presentation::Hero);publicCard_->hide();layout->insertWidget(2,publicCard_);
    publicCard_->informationButton()->setObjectName("assetDetailInfo");publicCard_->findChild<QWidget*>("metricSparkline")->setObjectName("assetDetailSparkline");
    indicatorSlots_=new QWidget(content);indicatorSlots_->setObjectName("assetDetailIndicators");auto* indicatorLayout=new QHBoxLayout(indicatorSlots_);indicatorLayout->setContentsMargins(0,0,0,0);
    for(int i=0;i<3;++i){auto* label=plainLabel(QString("assetDetailIndicator%1").arg(i),indicatorSlots_);label->setProperty("type","caption");label->hide();indicators_.append(label);indicatorLayout->addWidget(label,1);}indicatorSlots_->hide();layout->insertWidget(3,indicatorSlots_);
    chartLink_=new QPushButton(tr("차트에서 보기"),content);chartLink_->setObjectName("assetDetailChart");chartLink_->hide();layout->insertWidget(4,chartLink_);connect(chartLink_,&QPushButton::clicked,this,[this]{if(publicPresentation_&&!chartIdentity_.isEmpty())emit publicChartRequested(chartIdentity_);});
    favourite_ = new QPushButton(tr("☆ 관심 리스트"), content);
    favourite_->setObjectName("publicFavourite");
    favourite_->setCheckable(true);
    favourite_->setProperty("compact", true);
    layout->insertWidget(2, favourite_, 0, Qt::AlignLeft);
    connect(favourite_, &QPushButton::toggled, this, [this] { if (!publicId_.isEmpty()) emit favouriteRequested(publicSection_, publicId_); });
    setPublicSelection({}, {}, false);
}
void AssetDetailPanel::setPublicSelection(const QString& section, const QString& id, bool saved) {
    publicSection_ = section; publicId_ = id;
    if (!favourite_) return;
    favourite_->setEnabled(!id.isEmpty());
    const QSignalBlocker block(favourite_);
    favourite_->setChecked(saved);
    favourite_->setText(saved ? tr("★ 관심 리스트에 저장됨") : tr("☆ 관심 리스트에 추가"));
}

void AssetDetailPanel::showMissing(const PortfolioMissingView& missing) {
    selected_ = true;
    const auto status = missing.active ? tr("조회 중 · 이 계좌의 자산은 아직 표시하지 않았습니다.") : tr("미조회·확인 필요 · 포함 자산 수를 알 수 없습니다.");
    setBody(missing.alias, connectionMarketName(missing.market), missing.active ? tr("조회 중") : tr("확인 필요"), status, QStringLiteral("warning"), status,
        {{tr("현재 단계"), missing.stage.isEmpty() ? tr("조회 대기") : missing.stage}},
        {tr("현재 단계  %1").arg(missing.stage.isEmpty() ? tr("조회 대기") : missing.stage)});
}

void AssetDetailPanel::clearDetail() {
    selected_ = false;
    setBody({}, {}, {}, {}, {}, {}, {}, {});
    setPublicSelection({}, {}, false);
}

void AssetDetailPanel::showPublicCard(const ExploreMetricCardData& data){
    publicPresentation_=true;publicCard_->setData(data);publicCard_->show();
    for(auto* widget:QList<QWidget*>{title_,subtitle_,headline_,headlineSub_,evidence_,blocks_,connections_})widget->hide();
    // Mockup fact rows sit under the public card; legacy holding rows from other bodies stay hidden.
    places_->setVisible(publicFacts_&&placesLayout_->count()>0);settleLayout();
}
void AssetDetailPanel::setReferenceNumbers(const QList<int>& numbers){
    references_->clear();references_->hide();
    if(publicCard_)publicCard_->setEvidenceNumbers(numbers);
}
void AssetDetailPanel::setValuationEvidenceNumbers(const QList<int>& numbers){
    // The parent screen owns provenance in this mode. Clearing the entries,
    // rather than only hiding the widget, also survives a theme repaint.
    evidenceFooter_->setEntries({});setReferenceNumbers(numbers);
}
void AssetDetailPanel::setPublicEvidence(const QList<ExploreMetricEvidence>& entries){
    evidenceFooter_->setEntries(entries);if(publicCard_&&publicPresentation_){auto data=publicCard_->data();data.evidence=entries;publicCard_->setData(data);}
    setReferenceNumbers(evidenceFooter_->numbers());
}
void AssetDetailPanel::setPublicHistory(const ExploreMetricHistory& history){if(!publicCard_||!publicPresentation_)return;auto data=publicCard_->data();data.history=history;publicCard_->setData(data);}
void AssetDetailPanel::setPublicHistoryContext(const QString& key,const QString& unit){if(!publicCard_||!publicPresentation_)return;auto data=publicCard_->data();if(data.sourceKey!=key||data.unit!=unit)data.history={};data.sourceKey=key;data.unit=unit;publicCard_->setData(data);}
void AssetDetailPanel::setPrivateLinkage(bool allowed,const QString& information){if(!publicCard_)return;if(!allowed){if(privateLinkageAllowed_)publicCard_->clearPrivate();privateLinkageAllowed_=false;return;}privateLinkageAllowed_=true;publicCard_->setPrivateAccess(true);publicCard_->setPrivateInformation(usabilityPublicText(information));}
void AssetDetailPanel::setPublicIndicators(const QList<PublicDetailIndicator>& values){if(!indicatorSlots_)return;bool any=false;for(int i=0;i<indicators_.size();++i){auto* label=indicators_[i];const bool show=i<values.size()&&!values[i].value.isEmpty();const auto direction=show?values[i].direction:MovementDirection::Unknown;const auto marker=MovementColorPolicy::marker(direction);label->setText(show?values[i].name+"\n"+(marker.isEmpty()?QString{}:marker+" ")+values[i].value:QString{});const QString tone=direction==MovementDirection::Up?"up":direction==MovementDirection::Down?"down":"neutral";if(label->property("tone").toString()!=tone){label->setProperty("tone",tone);label->style()->unpolish(label);label->style()->polish(label);}label->setVisible(show);any|=show;}indicatorSlots_->setVisible(any);settleLayout();}
void AssetDetailPanel::setChartIdentity(const QString& identity){chartIdentity_=publicPresentation_&&WatchlistWorkspaceList::validIdentity(WatchlistWorkspaceList::identityFromKey(identity))?identity:QString{};if(chartLink_)chartLink_->setVisible(!chartIdentity_.isEmpty());settleLayout();}

ListDetailView::ListDetailView(QWidget* list, AssetDetailPanel* detail, QWidget* parent)
    : QWidget(parent), list_(list), detail_(detail) {
    setObjectName("assetListDetail");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName("assetListDetailSplitter");
    splitter_->setChildrenCollapsible(false);
    splitter_->addWidget(list);
    splitter_->addWidget(detail);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    layout->addWidget(splitter_);
    connect(detail, &AssetDetailPanel::backRequested, this, &ListDetailView::showList);
    applyMode();
}

void ListDetailView::showDetail() { detailShown_ = true; applyMode(); }
void ListDetailView::showList() {
    // Focus moves to the list only out of the closing detail (its back
    // button). A refresh or a filter must not pull focus from the search
    // field or the header; on a list view that focus would also make the
    // first row current without any change signal.
    const bool focusInDetail = detail_ && detail_->isAncestorOf(QApplication::focusWidget());
    detailShown_ = false;
    applyMode();
    if (list_ && focusInDetail) list_->setFocus(Qt::OtherFocusReason);
}

void ListDetailView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const bool next = event->size().width() < NarrowWidth;
    if (next != narrow_) { narrow_ = next; applyMode(); }
    // Start wide layouts at 60:40; afterwards keep the user's divider position.
    if (!narrow_ && !sized_ && event->size().width() > 0) {
        sized_ = true;
        const int width = event->size().width();
        splitter_->setSizes({width * 3 / 5, width - width * 3 / 5});
    }
}

void ListDetailView::applyMode() {
    if (!list_ || !detail_) return;
    detail_->back_->setVisible(narrow_);
    if (!narrow_) {
        list_->show();
        const bool show = !detailOnlyWithSelection_ || detail_->hasSelection();
        // The splitter keeps no width for a hidden detail, so a detail that
        // comes back would stay 0 px wide. Remember its share while it is
        // shown and give it back when a selection shows it again.
        if (detailOnlyWithSelection_ && !show && !detail_->isHidden()) rememberDetailShare();
        const bool appearing = detailOnlyWithSelection_ && show && detail_->isHidden();
        detail_->setVisible(show);
        if (appearing) restoreDetailShare();
        return;
    }
    list_->setVisible(!detailShown_);
    detail_->setVisible(detailShown_);
}

void ListDetailView::rememberDetailShare() { detailShare_.remember(*splitter_); }

void ListDetailView::restoreDetailShare() {
    const int width = SplitterShare::extent(*splitter_);
    if (width <= 0) return;
    const int floor = qMin(DetailMinimumWidth, width / 2);
    detailShare_.restore(*splitter_, floor, floor);
}

} // namespace keefi
