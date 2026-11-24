#include "keefi/AssetShareDialog.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/LocalCheckBox.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QTimeZone>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace keefi {
struct AssetShareDialog::Private {
    AssetShareDialog* q=nullptr;
    AssetShareSnapshot snapshot;
    AssetShareOptions options;
    PathChooser chooser;
    AssetShareRenderResult rendered;
    QLabel *preview=nullptr,*status=nullptr,*privacy=nullptr;
    QCheckBox *quantity=nullptr,*valuation=nullptr,*custody=nullptr,*today=nullptr;
    QLabel* todayReason=nullptr;
    QComboBox *market=nullptr,*period=nullptr;
    QList<WatchlistIdentity> markets;
    QPushButton* save=nullptr;
    bool saving=false,personalRevoked=false;
    quint64 generation=0;
    bool available() const { return rendered.available&&!rendered.image.isNull(); }
    void controls() {
        save->setEnabled(!saving&&available());
        market->setEnabled(!saving&&markets.size()>1);period->setEnabled(!saving);
        const bool allowed=!saving&&!personalRevoked&&snapshot.personal.has_value();
        quantity->setEnabled(allowed&&!snapshot.personal->quantity.isEmpty());
        valuation->setEnabled(allowed&&!snapshot.personal->currentValuation.isEmpty());
        custody->setEnabled(allowed&&!snapshot.personal->custodyProviders.isEmpty());
        today->setEnabled(allowed&&snapshot.personal->today.has_value());
        todayReason->setText(snapshot.personal&&snapshot.personal->today?QObject::tr("입출금 포함 가능 · 오늘 0시 이후의 관측만 비교해요."):
            snapshot.personal&&!snapshot.personal->todayUnavailableReason.isEmpty()?snapshot.personal->todayUnavailableReason:
            QObject::tr("오늘 비교를 확인할 수 없어 관측 평가액 증감은 포함할 수 없어요."));
    }
    void showPreview() {
        if(!available()){preview->clear();preview->setAccessibleDescription({});return;}
        const auto size=preview->contentsRect().size();
        if(size.width()>0&&size.height()>0)preview->setPixmap(QPixmap::fromImage(rendered.image).scaled(size,Qt::KeepAspectRatio,Qt::SmoothTransformation));
        preview->setAccessibleDescription(rendered.captions.join('\n'));
    }
    void refresh() {
        rendered=AssetShareCard::render(snapshot,options);controls();showPreview();
        privacy->setText((options.includeQuantity||options.includeValuation||options.includeCustody||options.includeTodayObservation)?QObject::tr("선택한 개인값이 이미지에 포함돼요 · 선택한 로컬 파일에만 저장"):
            QObject::tr("시장 가격 변화 · 계좌 금액·수량은 기본 숨김"));
        status->setText(available()?QObject::tr("공개 가격의 정적 미리보기 · PNG 저장을 누르면 로컬 경로를 선택합니다."):
            rendered.reason);
    }
    void resetConsent() {
        options.includeQuantity=options.includeValuation=options.includeCustody=options.includeTodayObservation=false;
        for(auto* box:{quantity,valuation,custody,today}){const QSignalBlocker block(box);box->setChecked(false);}
    }
    void invalidatePublic() {
        ++generation;snapshot.market={};snapshot.requestedFromMs=snapshot.requestedToMs=0;resetConsent();refresh();
    }
    void failed() {
        const auto reason=QObject::tr("PNG를 저장하지 못했습니다. 선택한 로컬 PNG 경로와 쓰기 권한을 확인해 주세요.");
        status->setText(reason);emit q->saveFailed(reason);
    }
    void saveClicked() {
        if(saving||!available()||!q->isVisible())return;
        const auto input=snapshot;const auto settings=options;const auto choose=chooser;const auto epoch=generation;
        QPointer<AssetShareDialog> alive(q);saving=true;controls();QString path;bool chooserFailed=false;
        try{path=choose?choose(q):QFileDialog::getSaveFileName(q,QObject::tr("PNG 저장"),QStringLiteral("KEEFI-%1-%2.png").arg(input.assetSymbol,QDateTime::fromMSecsSinceEpoch(input.market.asOfMs,QTimeZone("Asia/Seoul")).toString("yyyyMMdd")),QObject::tr("PNG 이미지 (*.png)"));}
        catch(...){chooserFailed=true;}
        if(!alive)return;
        saving=false;controls();
        // Lock/revocation or a new quote invalidates the old chooser snapshot.
        if(!q->isVisible()||generation!=epoch)return;
        if(chooserFailed){failed();return;}
        if(path.isEmpty())return;
        QString error;if(!AssetShareCard::savePng(input,settings,path,&error)){failed();return;}
        status->setText(QObject::tr("PNG 저장 완료 · 선택한 로컬 파일에만 저장했습니다."));emit q->saved(path);
    }
};
AssetShareDialog::AssetShareDialog(AssetShareSnapshot snapshot, AssetShareOptions options, QWidget* parent)
    : QDialog(parent), d_(std::make_unique<Private>()) {
    d_->q=this;d_->snapshot=std::move(snapshot);d_->options=options;
    setObjectName("assetShareDialog");setWindowTitle(tr("자산 카드 · 로컬 PNG"));setFont(KeefiFonts::font(FontRole::Body15));resize(1100,760);
    auto* columns=new QHBoxLayout(this);columns->setSpacing(18);
    auto* previewColumn=new QVBoxLayout;columns->addLayout(previewColumn,1);
    auto* optionsPanel=new QWidget(this);optionsPanel->setObjectName("assetShareOptions");optionsPanel->setMinimumWidth(340);optionsPanel->setMaximumWidth(410);
    auto* layout=new QVBoxLayout(optionsPanel);columns->addWidget(optionsPanel);
    d_->privacy=new QLabel(this);d_->privacy->setObjectName("assetSharePrivacy");d_->privacy->setTextFormat(Qt::PlainText);
    d_->privacy->setFont(KeefiFonts::font(FontRole::Caption12));d_->privacy->setWordWrap(true);previewColumn->addWidget(d_->privacy);
    d_->preview=new QLabel(this);d_->preview->setObjectName("assetSharePreview");d_->preview->setAccessibleName(tr("자산 가격 변화 카드 미리보기"));
    d_->preview->setAlignment(Qt::AlignCenter);d_->preview->setMinimumSize(440,400);d_->preview->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);previewColumn->addWidget(d_->preview,1);
    d_->status=new LedgerTextLabel(FontRole::Secondary14,this);d_->status->setObjectName("assetShareStatus");
    d_->status->setTextFormat(Qt::PlainText);d_->status->setWordWrap(true);previewColumn->addWidget(d_->status);
    auto* selectors=new QHBoxLayout;
    d_->period=new QComboBox(this);d_->period->setObjectName("assetSharePeriod");d_->period->setAccessibleName(tr("카드 기간"));d_->period->setMinimumHeight(44);
    for(const auto period:{AssetSharePeriod::Day,AssetSharePeriod::Week,AssetSharePeriod::Month,AssetSharePeriod::HalfYear})
        d_->period->addItem(AssetShareCard::periodLabel(period),int(period));
    d_->period->setCurrentIndex(d_->period->findData(int(d_->snapshot.period)));selectors->addWidget(d_->period);
    d_->market=new QComboBox(this);d_->market->setObjectName("assetShareMarket");d_->market->setAccessibleName(tr("공개 시세 시장"));d_->market->setMinimumHeight(44);selectors->addWidget(d_->market,1);layout->addLayout(selectors);
    const auto personal=[&](const QString& label,const char* name,bool AssetShareOptions::* option){
        auto* box=new LocalCheckBox(label,this);box->setObjectName(name);box->setMinimumSize(44,44);box->setProperty("fullRowTarget",true);box->setChecked(d_->options.*option);layout->addWidget(box);
        connect(box,&QCheckBox::toggled,this,[this,box,option](bool enabled){
            if(d_->saving||d_->personalRevoked){const QSignalBlocker block(box);box->setChecked(d_->options.*option);return;}
            d_->options.*option=enabled;d_->refresh();});return box;
    };
    d_->quantity=personal(tr("현재 수량"),"assetShareQuantity",&AssetShareOptions::includeQuantity);
    d_->valuation=personal(tr("현재 평가액"),"assetShareValuation",&AssetShareOptions::includeValuation);
    d_->custody=personal(tr("보관처 표시"),"assetShareCustody",&AssetShareOptions::includeCustody);
    d_->today=personal(tr("오늘 관측 평가액 증감"),"assetShareTodayObservation",&AssetShareOptions::includeTodayObservation);
    d_->todayReason=new QLabel(this);d_->todayReason->setObjectName("assetShareTodayReason");d_->todayReason->setTextFormat(Qt::PlainText);d_->todayReason->setWordWrap(true);layout->addWidget(d_->todayReason);
    connect(d_->market,&QComboBox::activated,this,[this](int index){if(index<0||index>=d_->markets.size())return;
        const auto identity=d_->markets[index];d_->invalidatePublic();emit marketRequested(identity);});
    connect(d_->period,&QComboBox::activated,this,[this](int index){const auto period=AssetSharePeriod(d_->period->itemData(index).toInt());
        d_->snapshot.period=period;d_->invalidatePublic();emit periodRequested(period);});
    layout->addStretch();auto* actions=new QHBoxLayout;actions->addStretch();
    d_->save=new QPushButton(tr("PNG 저장"),this);d_->save->setObjectName("assetShareSave");d_->save->setMinimumSize(44,44);actions->addWidget(d_->save);
    auto* close=new QPushButton(tr("닫기"),this);close->setObjectName("assetShareClose");close->setMinimumSize(44,44);actions->addWidget(close);layout->addLayout(actions);
    connect(close,&QPushButton::clicked,this,&QDialog::reject);
    connect(d_->save,&QPushButton::clicked,this,[this]{d_->saveClicked();});
    d_->refresh();
}
AssetShareDialog::~AssetShareDialog() = default;
AssetShareSnapshot AssetShareDialog::snapshot() const { return d_->snapshot; }
AssetShareOptions AssetShareDialog::options() const { return d_->options; }
QImage AssetShareDialog::previewImage() const { return d_->available()?d_->rendered.image:QImage{}; }
bool AssetShareDialog::previewAvailable() const { return d_->available(); }
void AssetShareDialog::setPathChooser(PathChooser chooser) { d_->chooser=std::move(chooser); }
void AssetShareDialog::setSnapshot(AssetShareSnapshot value) {
    ++d_->generation;if(d_->personalRevoked)value.personal.reset();d_->snapshot=std::move(value);d_->resetConsent();
    {const QSignalBlocker block(d_->period);d_->period->setCurrentIndex(d_->period->findData(int(d_->snapshot.period)));}
    {const QSignalBlocker block(d_->market);d_->market->setCurrentIndex(d_->markets.indexOf(d_->snapshot.market.identity));}d_->refresh();
}
void AssetShareDialog::setMarkets(const QList<WatchlistIdentity>& candidates, const WatchlistIdentity& selected) {
    ++d_->generation;const QSignalBlocker block(d_->market);d_->markets.clear();d_->market->clear();
    const QMap<QString,QString> sources{{"upbit","업비트"},{"binance","Binance Spot"},{"KR","한국 주식"},{"US","미국 주식"}};
    for(const auto& candidate:candidates)if(WatchlistWorkspaceList::validIdentity(candidate)&&sources.contains(candidate.source)&&!d_->markets.contains(candidate)){
        d_->markets.append(candidate);d_->market->addItem(sources.value(candidate.source)+" "+candidate.code);}
    d_->market->setCurrentIndex(d_->markets.indexOf(selected));
    if(selected!=d_->snapshot.market.identity||!d_->markets.contains(selected))d_->invalidatePublic();else d_->controls();
}
void AssetShareDialog::setPublicStatus(const QString& fixedPublicText) {
    if(!d_->available()&&!d_->saving)d_->status->setText(fixedPublicText);
}
void AssetShareDialog::clearPersonalValues() {
    d_->personalRevoked=true;++d_->generation;d_->snapshot.personal.reset();d_->resetConsent();d_->refresh();
}
void AssetShareDialog::resizeEvent(QResizeEvent* event) { QDialog::resizeEvent(event);if(d_&&d_->preview)d_->showPreview(); }
}
