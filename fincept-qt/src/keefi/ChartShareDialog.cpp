#include "keefi/ChartShareDialog.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/LocalCheckBox.h"
#include <QCheckBox>
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
struct ChartShareDialog::Private {
    ChartShareDialog* q=nullptr;
    ChartShareSnapshot snapshot;
    ChartShareOptions options;
    PathChooser chooser;
    ChartShareRenderResult rendered;
    QLabel *preview=nullptr,*status=nullptr;
    QCheckBox* drawings=nullptr;
    QPushButton* save=nullptr;
    bool saving=false;
    bool available()const{return options.maskPersonalValues&&rendered.available&&!rendered.image.isNull();}
    bool canSave()const{return available()&&!snapshot.closed.isEmpty()&&snapshot.source.receivedAtMs>0;}
    void showPreview(){
        if(!preview)return;
        if(!available()){preview->clear();preview->setAccessibleDescription({});return;}
        const auto size=preview->contentsRect().size();
        if(size.width()>0&&size.height()>0)preview->setPixmap(QPixmap::fromImage(rendered.image).scaled(size,Qt::KeepAspectRatio,Qt::SmoothTransformation));
        preview->setAccessibleDescription(rendered.publicCaptions.join('\n'));
    }
    void controls(){
        if(save)save->setEnabled(!saving&&canSave());
        if(drawings)drawings->setEnabled(!saving&&available()&&!snapshot.drawings.isEmpty());
    }
    void refresh(){
        rendered=ChartShareCard::render(snapshot,options);controls();showPreview();
        status->setText(!available()?QObject::tr("카드를 만들 수 없습니다. 공개 차트 자료 또는 저장 설정을 확인해 주세요."):
            canSave()?QObject::tr("공개 자료의 정적 미리보기 · PNG 저장을 누르면 로컬 경로를 선택합니다."):
            QObject::tr("완료된 공개 캔들 자료가 없어 저장할 수 없습니다."));
    }
    void refreshPreviewInk(){
        if(!available())return;
        const auto next=ChartShareCard::render(snapshot,options);
        if(!next.available||next.image.isNull())return;
        // A theme is presentation only. Preserve the public snapshot, options,
        // captions, save status and any nested chooser session verbatim.
        rendered.image=next.image;showPreview();
    }
    void failed(){
        const auto reason=QObject::tr("PNG를 저장하지 못했습니다. 선택한 로컬 PNG 경로와 쓰기 권한을 확인해 주세요.");
        status->setText(reason);emit q->saveFailed(reason);
    }
    void saveClicked(){
        if(saving||!canSave()||!q->isVisible())return;
        // Freeze exactly the rendered public input before opening a nested native
        // chooser. Neither a caller mutation nor a checkbox event changes it.
        const auto input=snapshot;const auto settings=options;const auto choose=chooser;
        QPointer<ChartShareDialog> alive(q);saving=true;controls();QString path;bool chooserFailed=false;
        try{path=choose?choose(q):QFileDialog::getSaveFileName(q,QObject::tr("PNG 저장"),QStringLiteral("KEEFI-chart.png"),QObject::tr("PNG 이미지 (*.png)"));}
        catch(...){chooserFailed=true;}
        if(!alive)return;
        saving=false;controls();
        if(!q->isVisible())return;
        if(chooserFailed){failed();return;}
        if(path.isEmpty())return; // Cancellation never attempts a write.
        QString error;
        if(!ChartShareCard::savePng(input,settings,path,&error)){failed();return;}
        status->setText(QObject::tr("PNG 저장 완료 · 선택한 로컬 파일에만 저장했습니다."));emit q->saved(path);
    }
};
ChartShareDialog::ChartShareDialog(ChartShareSnapshot snapshot,ChartShareOptions options,QWidget* parent)
    :QDialog(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->snapshot=std::move(snapshot);d_->options=options;setObjectName("chartShareDialog");setWindowTitle(tr("자랑 카드 · 로컬 PNG"));resize(900,740);
    auto* layout=new QVBoxLayout(this);
    auto* privacy=new QLabel(tr("계좌 금액·수량 제외 · 로컬 저장만"),this);privacy->setObjectName("chartSharePrivacy");privacy->setTextFormat(Qt::PlainText);privacy->setFont(KeefiFonts::font(FontRole::Caption12));layout->addWidget(privacy);
    auto* preview=new QLabel(this);d_->preview=preview;preview->setObjectName("chartSharePreview");preview->setAccessibleName(tr("공개 차트 PNG 미리보기"));preview->setAlignment(Qt::AlignCenter);preview->setMinimumSize(640,400);preview->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);layout->addWidget(preview,1);
    auto* status=new LedgerTextLabel(FontRole::Secondary14,this);d_->status=status;status->setObjectName("chartShareStatus");status->setTextFormat(Qt::PlainText);status->setWordWrap(true);layout->addWidget(status);
    auto* drawings=new LocalCheckBox(tr("저장한 공개 그림 포함"),this);d_->drawings=drawings;drawings->setObjectName("chartShareDrawings");drawings->setMinimumSize(44,44);drawings->setProperty("fullRowTarget",true);drawings->setChecked(options.includePublicDrawings);drawings->setToolTip(d_->snapshot.drawings.isEmpty()?tr("포함할 수 있는 저장된 공개 그림이 없습니다."):tr("직접 켠 경우에만 이 공개 그림을 카드에 포함합니다."));layout->addWidget(drawings);
    auto* actions=new QHBoxLayout;actions->addStretch();auto* save=new QPushButton(tr("PNG 저장"),this);d_->save=save;save->setObjectName("chartShareSave");save->setMinimumSize(44,44);actions->addWidget(save);
    auto* close=new QPushButton(tr("닫기"),this);close->setObjectName("chartShareClose");close->setMinimumSize(44,44);actions->addWidget(close);layout->addLayout(actions);connect(close,&QPushButton::clicked,this,&QDialog::reject);
    connect(save,&QPushButton::clicked,this,[this]{d_->saveClicked();});
    connect(drawings,&QCheckBox::toggled,this,[this](bool enabled){
        if(d_->saving){const QSignalBlocker block(d_->drawings);d_->drawings->setChecked(d_->options.includePublicDrawings);return;}
        d_->options.includePublicDrawings=enabled;d_->refresh();
    });
    d_->refresh();
    connect(KeefiTheme::instance(),&KeefiTheme::changed,this,[this]{d_->refreshPreviewInk();});
}
ChartShareDialog::~ChartShareDialog()=default;
ChartShareSnapshot ChartShareDialog::snapshot()const{return d_->snapshot;}
ChartShareOptions ChartShareDialog::options()const{return d_->options;}
QImage ChartShareDialog::previewImage()const{return d_->available()?d_->rendered.image:QImage{};}
bool ChartShareDialog::previewAvailable()const{return d_->available();}
void ChartShareDialog::setPathChooser(PathChooser chooser){d_->chooser=std::move(chooser);}
void ChartShareDialog::resizeEvent(QResizeEvent* event){QDialog::resizeEvent(event);if(d_)d_->showPreview();}
}
