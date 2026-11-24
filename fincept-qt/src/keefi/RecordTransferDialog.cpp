#include "keefi/RecordTransferDialog.h"
#include "keefi/ProductIdentity.h"
#include "keefi/RecordTransfer.h"
#include "keefi/PortfolioController.h"
#include "keefi/SwapJournal.h"
#include "keefi/Vault.h"
#include "keefi/LocalCheckBox.h"
#include "keefi/LocalTableWidget.h"
#include "keefi/SecretInput.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/LedgerTextHighlighter.h"
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace keefi {
namespace {
QLabel* textLabel(const QString& text, QWidget* owner, const char* name, FontRole role=FontRole::Secondary14) {
    auto* label=new LedgerTextLabel(text,role,owner);label->setObjectName(name);label->setTextFormat(Qt::PlainText);label->setWordWrap(true);return label;
}
QString stringValue(const QJsonObject& object,const char* key) {
    const auto value=object.value(QLatin1String(key));
    return value.isString()?value.toString():value.isDouble()?QString::number(value.toDouble(),'f',0):QString();
}
QString timestamp(const QJsonObject& payload) {
    for(const char* key:{"timeMs","observedAtMs","createdAt"}) {
        const auto value=stringValue(payload,key);bool valid=false;const auto ms=value.toLongLong(&valid);
        if(valid&&ms>0)return QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss");
    }
    return QStringLiteral("미상");
}
QString kindLabel(const QString& kind) {
    if(kind=="snapshot")return QStringLiteral("자산 스냅샷");
    if(kind=="event")return QStringLiteral("자산 사건");
    if(kind=="order")return QStringLiteral("주문 관찰");
    if(kind=="swap")return QStringLiteral("스왑 관찰");
    if(kind=="pending")return QStringLiteral("미확정 관찰");
    return QStringLiteral("미상");
}
struct TransferJob {
    enum class Mode { Export, Import } mode;
    QString path,error;
    SecureBuffer password;
    RecordTransferBundle bundle;
    RecordTransferSelection selection;
    RecordTransferCancellation cancel=std::make_shared<std::atomic_bool>(false);
    quint64 generation=0,serial=0;
    bool success=false,exportFilePresent=false;
    explicit TransferJob(Mode value):mode(value){}
};
}
struct RecordTransferDialog::Private {
    RecordTransferDialog* q;Vault& vault;PortfolioController* portfolio;SwapJournal* swap;
    RecordTransfer transfer;QThread* worker=nullptr;QTimer* checkpoint=nullptr;
    std::shared_ptr<TransferJob> pending;
    RecordTransferCancellation cancellation;
    quint64 serial=0;bool busy=false,exportActive=false;int page=0;
    QJsonArray displayed;
    QPushButton *prepare,*exportSubmit,*importSubmit,*cancel,*previous,*next,*detailsToggle;
    QPlainTextEdit* details;
    QLineEdit *exportPath,*exportPassword,*exportConfirm,*importPath,*importPassword;
    QCheckBox *snapshots,*events,*orders,*swaps,*pendingRecords;
    QLabel *status,*pageState;QTableWidget* records;QTabWidget* tabs;
    Private(RecordTransferDialog* owner,Vault& v,PortfolioController* p,SwapJournal* s):q(owner),vault(v),portfolio(p),swap(s),transfer(v){}
    void clearPasswords() {
        for(auto* edit:{exportPassword,exportConfirm,importPassword}){edit->setEchoMode(QLineEdit::Password);edit->setText({});}
    }
    void render() {
        const bool ready=vault.isUnlocked()&&transfer.isOpen();
        prepare->setEnabled(vault.isUnlocked()&&!busy);prepare->setText(transfer.exists()?QStringLiteral("기록 저장소 열기"):QStringLiteral("기록 이동 준비"));
        QString error;
        const bool selected=snapshots->isChecked()||events->isChecked()||orders->isChecked()||swaps->isChecked()||pendingRecords->isChecked();
        exportSubmit->setEnabled(ready&&!busy&&selected&&!exportPath->text().isEmpty()
            &&Vault::validateNewPassword(exportPassword->text(),&error)&&exportPassword->text()==exportConfirm->text()
            &&!exceedsSecretByteLimit(exportPassword->text(),Vault::MaxPasswordBytes));
        importSubmit->setEnabled(ready&&!busy&&!importPath->text().isEmpty()&&!importPassword->text().isEmpty()
            &&!exceedsSecretByteLimit(importPassword->text(),Vault::MaxPasswordBytes));
        cancel->setEnabled(busy);
        for(auto* edit:{exportPath,exportPassword,exportConfirm,importPath,importPassword})edit->setEnabled(!busy);
        for(auto* box:{snapshots,events,orders,swaps,pendingRecords})box->setEnabled(!busy);
    }
    void renderRecords() {
        records->setRowCount(0);const int start=page*200,end=qMin(start+200,int(displayed.size()));
        for(int index=start;index<end;++index){
            const auto record=displayed.at(index).toObject(),p=record.value("payload").toObject();const auto kind=record.value("kind").toString();
            QStringList assets,quantities;
            if(kind=="snapshot")for(const auto& component:p.value("components").toArray()){
                const auto c=component.toObject();assets.append(stringValue(c,"asset"));quantities.append(stringValue(c,"quantity"));
            }
            else if(kind=="swap"){assets={stringValue(p,"inputMint"),stringValue(p,"outputMint")};quantities={stringValue(p,"inputAtomic")+" atomic"};}
            else{assets={stringValue(p,kind=="pending"?"symbol":"asset")};quantities={stringValue(p,"quantity")};if(!stringValue(p,"quantityUnit").isEmpty())quantities[0]+=" "+stringValue(p,"quantityUnit");}
            QString official=stringValue(p,"officialId");if(official.isEmpty())official=stringValue(p,"orderId");if(official.isEmpty())official=stringValue(p,"transactionId");
            auto state=stringValue(p,"status");if(state.isEmpty())state=stringValue(p,"state");
            if(kind=="snapshot")state=p.value("balanceComplete").toBool()?QStringLiteral("조회 범위 확인"):QStringLiteral("불완전");
            const QStringList cells{kindLabel(kind),stringValue(p,"provider"),stringValue(p,"market"),stringValue(p,"environment"),timestamp(p),
                assets.join(" / "),quantities.join(" / "),state,official,stringValue(p,"accountScope")};
            const int row=records->rowCount();records->insertRow(row);
            for(int column=0;column<cells.size();++column)records->setItem(row,column,new QTableWidgetItem(cells[column].isEmpty()?QStringLiteral("미상"):cells[column]));
        }
        previous->setEnabled(page>0);next->setEnabled(end<displayed.size());
        pageState->setText(QStringLiteral("%1–%2 / %3개 기록 · 페이지당 200개").arg(displayed.isEmpty()?0:start+1).arg(end).arg(displayed.size()));
    }
    void cancelOperation(bool locking=false) {
        const bool exporting=busy&&exportActive,waiting=bool(pending);
        ++serial;if(cancellation)cancellation->store(true);checkpoint->stop();pending.reset();cancellation.reset();busy=false;
        clearPasswords();displayed={};details->clear();details->hide();detailsToggle->hide();page=0;transfer.lock();renderRecords();render();
        status->setText(locking?QStringLiteral("금고 잠금으로 기록과 암호를 지웠습니다. 진행 중 결과는 적용하지 않습니다."):
            QStringLiteral("취소했습니다. 기존 기록은 보존하며 진행 중 결과는 적용하지 않습니다."));
        if(exporting)status->setText(status->text()+(waiting?QStringLiteral("\n저장 대기 중 취소하여 새 패키지 파일을 만들지 않았습니다."):
            QStringLiteral("\n저장 경계를 이미 통과한 새 암호화 파일은 남을 수 있습니다. 작업 종료 후 결과를 확인하세요.")));
    }
    bool openExisting() {
        QString error;if(!vault.isUnlocked()){status->setText("금고 잠금 해제가 필요합니다.");render();return false;}
        if(!transfer.isOpen()&&!transfer.open(&error)){status->setText(error+QStringLiteral("\n처음 사용하는 경우 ‘기록 이동 준비’를 누르세요."));render();return false;}
        displayed=transfer.records();page=0;renderRecords();render();return true;
    }
    void finish(const std::shared_ptr<TransferJob>& job) {
        // A previous worker may finish after a cancelled operation and a new start.
        if(job->serial!=serial||job->cancel->load()||!vault.isUnlocked()||job->generation!=transfer.generation()){
            if(job->cancel->load()&&job->mode==TransferJob::Mode::Export&&serial==job->serial+1&&!busy)
                status->setText(QStringLiteral("내보내기 취소·잠금 요청 후 작업이 종료되었습니다. 기존 기록은 보존됩니다.\n")+
                    (job->exportFilePresent?QStringLiteral("선택 경로에 파일이 남아 있습니다. 저장 경계를 통과한 암호화 패키지일 수 있으며 자동 삭제하지 않습니다."):
                        QStringLiteral("작업 종료 시 선택 경로에 패키지 파일이 없습니다.")));
            job->bundle={};return;
        }
        busy=false;cancellation.reset();
        if(!job->success){status->setText(QStringLiteral("실패: %1\n기존 기록은 보존됩니다.").arg(job->error));
            if(job->mode==TransferJob::Mode::Export&&job->exportFilePresent)status->setText(status->text()+QStringLiteral("\n선택 경로에 파일이 남아 있지만 정상 저장 완료는 확인하지 못했습니다."));
            job->bundle={};render();return;}
        if(job->mode==TransferJob::Mode::Import){
            RecordTransferResult result;QString error;
            if(!transfer.commit(job->bundle,job->generation,&result,&error,job->cancel)){
                status->setText(QStringLiteral("가져오기 실패: %1\n기존 기록의 상태를 확인한 뒤 다시 여세요.").arg(error));
            }else{
                displayed=transfer.records();page=0;renderRecords();tabs->setCurrentIndex(2);
                status->setText(QStringLiteral("가져오기 완료 · 추가 %1 · 중복 %2 · 총 %3개\n읽기 전용 관찰 기록입니다. 현재 잔고·연결·진행 중 요청은 바꾸지 않았습니다.").arg(result.added).arg(result.duplicates).arg(result.total));
            }
        }else{
            status->setText(QStringLiteral("선택한 기록 %1개의 암호화 내보내기를 완료했습니다.\n선택하지 않은 범위와 시세 캐시는 포함하지 않았습니다. 수집 범위는 상세에서 확인하세요.").arg(job->bundle.records.size()));
            details->setPlainText(job->bundle.sourceSummary.join('\n'));detailsToggle->setVisible(!job->bundle.sourceSummary.isEmpty());
        }
        job->bundle={};render();
    }
    void run(const std::shared_ptr<TransferJob>& job) {
        // Only immutable snapshots, a move-only guarded password and a cancellation
        // token cross this boundary. No Vault, controller or widget is captured.
        worker=QThread::create([job]{
            try {
                QByteArray bytes;
                if(job->mode==TransferJob::Mode::Export)
                    job->success=RecordTransfer::encodePackage(job->bundle,job->password,&bytes,&job->error,job->cancel)
                        &&RecordTransfer::writePackageFile(job->path,bytes,&job->error,job->cancel);
                else job->success=RecordTransfer::readPackageFile(job->path,&bytes,&job->error,job->cancel)
                        &&RecordTransfer::decodePackage(bytes,job->password,&job->bundle,&job->error,job->cancel);
                job->password.clear();if(job->cancel->load())job->bundle={};
            }catch(...){job->password.clear();job->bundle={};job->error=QStringLiteral("암호 처리용 보호 메모리를 준비하지 못했습니다.");job->success=false;}
            if(job->mode==TransferJob::Mode::Export)job->exportFilePresent=QFileInfo::exists(job->path)||QFileInfo(job->path).isSymLink();
        });
        auto* thread=worker;
        QObject::connect(thread,&QThread::finished,q,[this,job,thread]{if(worker==thread)worker=nullptr;finish(job);});
        QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);
        thread->start();
    }
    void checkpointExport() {
        if(!pending)return;QString error;
        if(portfolio){
            const auto state=portfolio->checkpointRecords(&error);
            if(state==PortfolioController::RecordCheckpoint::Waiting){status->setText("기록의 저장 완료를 기다리고 있습니다. 취소할 수 있습니다.");return;}
            if(state==PortfolioController::RecordCheckpoint::Failed){checkpoint->stop();pending.reset();busy=false;cancellation.reset();status->setText(QStringLiteral("기록 저장 확인 실패: %1\n기존 기록은 보존됩니다.").arg(error));render();return;}
        }
        checkpoint->stop();
        auto job=std::move(pending);
        if(!transfer.collect(job->selection,portfolio?portfolio->historyLedger():nullptr,swap,&job->bundle,&error,portfolio?portfolio->latestLedger():nullptr)){
            busy=false;cancellation.reset();status->setText(QStringLiteral("내보내기 수집 실패: %1\n기존 기록은 보존됩니다.").arg(error));render();return;
        }
        status->setText("선택한 기록을 암호화하고 있습니다. 취소할 수 있습니다.");run(job);
    }
    void start(bool exporting) {
        if(busy||!vault.isUnlocked()||!transfer.isOpen())return;
        QString error;
        if(exporting&&(!Vault::validateNewPassword(exportPassword->text(),&error)||exportPassword->text()!=exportConfirm->text())){
            status->setText(error.isEmpty()?QStringLiteral("패키지 암호 확인이 일치하지 않습니다."):error);return;
        }
        const auto input=exporting?exportPassword:importPassword;
        if(input->text().isEmpty()||exceedsSecretByteLimit(input->text(),Vault::MaxPasswordBytes))return;
        auto job=std::make_shared<TransferJob>(exporting?TransferJob::Mode::Export:TransferJob::Mode::Import);
        job->selection={snapshots->isChecked(),events->isChecked(),orders->isChecked(),swaps->isChecked(),pendingRecords->isChecked()};
        job->path=exporting?exportPath->text():importPath->text();
        if(exporting&&QFileInfo::exists(job->path)){status->setText("기존 파일을 덮어쓸 수 없습니다. 새 파일 경로를 선택하세요.");clearPasswords();render();return;}
        try{job->password=SecureBuffer::fromUtf8(input->text());}catch(...){clearPasswords();status->setText("암호를 보호할 메모리를 준비하지 못했습니다.");render();return;}
        clearPasswords();details->clear();details->hide();detailsToggle->hide();job->generation=transfer.generation();job->serial=++serial;cancellation=job->cancel;busy=true;exportActive=exporting;render();
        if(exporting){pending=job;checkpoint->start();checkpointExport();}
        else{status->setText("패키지를 검증하고 복호화하고 있습니다. 취소할 수 있습니다.");run(job);}
    }
};

RecordTransferDialog::RecordTransferDialog(Vault& vault,PortfolioController* portfolio,SwapJournal* swap,QWidget* parent)
    :QDialog(parent),d_(std::make_unique<Private>(this,vault,portfolio,swap)) {
    setObjectName("recordTransferDialog");setWindowTitle(QStringLiteral("%1 · 암호화 기록 이동").arg(productName()));resize(960,760);setMinimumSize(720,620);
    auto* body=new QVBoxLayout(this);
    body->addWidget(textLabel("금융 키·Secret·토큰·금고·개인키·시드와 시세 캐시는 제외합니다. 수동 보정과 영구 설정은 지원하지 않습니다. 시세 캐시가 없어 과거 원화 평가를 완전히 복원하지 못할 수 있습니다.",this,"recordTransferScope"));
    d_->prepare=new QPushButton("기록 이동 준비",this);d_->prepare->setObjectName("prepareRecordTransfer");body->addWidget(d_->prepare);
    body->addWidget(textLabel("처음 준비하면 금고의 기록 키와 별도 빈 암호화 저장소를 만듭니다. 기존 저장소는 열기만 합니다.",this,"recordPreparationScope"));
    d_->tabs=new QTabWidget(this);body->addWidget(d_->tabs,1);
    auto* exportPage=new QWidget;auto* exportBody=new QVBoxLayout(exportPage);
    auto* selection=new QHBoxLayout;exportBody->addLayout(selection);
    auto checkbox=[&](const QString& text,const char* name){auto* box=new LocalCheckBox(text,exportPage);box->setObjectName(name);box->setChecked(true);selection->addWidget(box);connect(box,&QCheckBox::toggled,this,[this]{d_->render();});return box;};
    d_->snapshots=checkbox("자산 스냅샷","transferSnapshots");d_->events=checkbox("자산 사건","transferEvents");d_->orders=checkbox("주문 관찰","transferOrders");d_->swaps=checkbox("스왑 관찰","transferSwaps");d_->pendingRecords=checkbox("미확정 관찰","transferPending");
    auto line=[this](QWidget* page,const char* name,bool secret){auto* edit=new QLineEdit(page);edit->setObjectName(name);if(secret){edit->setEchoMode(QLineEdit::Password);edit->setMaxLength(int(Vault::MaxPasswordBytes)+1);edit->setInputMethodHints(Qt::ImhHiddenText|Qt::ImhNoPredictiveText|Qt::ImhNoAutoUppercase);}connect(edit,&QLineEdit::textChanged,this,[this]{d_->render();});return edit;};
    auto pathRow=[this](QLineEdit* edit,QWidget* page,bool save){auto* row=new QWidget(page);auto* layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->addWidget(edit,1);auto* choose=new QPushButton("경로 선택",row);choose->setObjectName(save?"chooseRecordExportPath":"chooseRecordImportPath");layout->addWidget(choose);connect(choose,&QPushButton::clicked,this,[this,edit,save]{
        if(d_->busy)return;const auto path=save?QFileDialog::getSaveFileName(this,"새 암호화 패키지",{},QStringLiteral("%1 기록 (*.keefirtx)").arg(productName()),nullptr,QFileDialog::DontConfirmOverwrite):QFileDialog::getOpenFileName(this,"암호화 패키지 선택",{},QStringLiteral("%1 기록 (*.keefirtx);;모든 파일 (*)").arg(productName()));if(!path.isEmpty())edit->setText(path);
    });return row;};
    auto* exportForm=new QFormLayout;exportForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);exportBody->addLayout(exportForm);
    d_->exportPath=line(exportPage,"exportRecordPath",false);exportForm->addRow("새 파일 경로",pathRow(d_->exportPath,exportPage,true));
    d_->exportPassword=line(exportPage,"exportRecordPassword",true);d_->exportConfirm=line(exportPage,"exportRecordConfirm",true);
    exportForm->addRow("패키지 암호",d_->exportPassword);exportForm->addRow("암호 확인",d_->exportConfirm);
    exportBody->addWidget(textLabel("암호는 12자 이상, 영문 대문자·소문자·숫자·특수문자를 각각 포함해야 합니다. UTF-8 1024바이트 이하이며 공백은 특수문자로 세지 않습니다. 기존 파일은 덮어쓰지 않습니다. 암호를 잃으면 패키지를 복구할 수 없습니다.",exportPage,"recordPasswordPolicy"));
    d_->exportSubmit=new QPushButton("선택한 기록 내보내기",exportPage);d_->exportSubmit->setObjectName("submitRecordExport");exportBody->addWidget(d_->exportSubmit);exportBody->addStretch();d_->tabs->addTab(exportPage,"내보내기");
    auto* importPage=new QWidget;auto* importBody=new QVBoxLayout(importPage);auto* importForm=new QFormLayout;importBody->addLayout(importForm);
    d_->importPath=line(importPage,"importRecordPath",false);importForm->addRow("패키지 경로",pathRow(d_->importPath,importPage,false));d_->importPassword=line(importPage,"importRecordPassword",true);importForm->addRow("패키지 암호",d_->importPassword);
    importBody->addWidget(textLabel("경로를 고르는 것만으로 가져오지 않습니다. 아래 버튼을 누르면 전체 검증 후 별도 기록 저장소에 추가하며 중복을 구분합니다. 현재 잔고·계좌 연결·진행 중 요청으로 넣거나 주문을 다시 실행하지 않습니다.",importPage,"recordImportScope"));
    d_->importSubmit=new QPushButton("검증 후 기록 가져오기",importPage);d_->importSubmit->setObjectName("submitRecordImport");importBody->addWidget(d_->importSubmit);importBody->addStretch();d_->tabs->addTab(importPage,"가져오기");
    auto* viewPage=new QWidget;auto* viewBody=new QVBoxLayout(viewPage);viewBody->addWidget(textLabel("가져온 기록은 읽기 전용입니다. 이관용 계좌 식별자는 이 PC 연결과 자동 연결되지 않으며 현재 잔고에 합산하지 않습니다. 미상 자료를 현재 환경으로 추정하지 않습니다.",viewPage,"recordReadOnlyScope"));
    d_->records=new LocalTableWidget(viewPage);d_->records->setObjectName("importedRecordTable");d_->records->setColumnCount(10);d_->records->setHorizontalHeaderLabels({"종류","원본 서비스","시장","환경","시각","종목","수량","상태","공식 ID","이관 계좌 식별자"});d_->records->setEditTriggers(QAbstractItemView::NoEditTriggers);d_->records->horizontalHeader()->setStretchLastSection(true);viewBody->addWidget(d_->records,1);
    auto* paging=new QHBoxLayout;d_->previous=new QPushButton("이전 200개",viewPage);d_->previous->setObjectName("previousRecordPage");d_->next=new QPushButton("다음 200개",viewPage);d_->next->setObjectName("nextRecordPage");d_->pageState=textLabel({},viewPage,"recordPageState",FontRole::Caption12);paging->addWidget(d_->previous);paging->addWidget(d_->pageState,1);paging->addWidget(d_->next);viewBody->addLayout(paging);d_->tabs->addTab(viewPage,"가져온 기록 보기");
    body->addWidget(textLabel("옮긴 기록은 조회용 과거 자료입니다. 새 PC에는 계좌 연결을 별도로 등록한 뒤 현재 잔고를 다시 조회하세요. 가져온 기록은 현재 잔고에 합산하지 않습니다.",this,"recordHandoverScope"));
    d_->status=textLabel({},this,"recordTransferStatus",FontRole::Body15);body->addWidget(d_->status);
    d_->detailsToggle=new QPushButton("수집 범위 상세",this);d_->detailsToggle->setObjectName("recordTransferDetailsToggle");d_->detailsToggle->hide();body->addWidget(d_->detailsToggle);
    d_->details=new QPlainTextEdit(this);d_->details->setObjectName("recordTransferDetails");d_->details->setReadOnly(true);d_->details->setFont(KeefiFonts::font(FontRole::Secondary14));new LedgerTextHighlighter(d_->details->document());d_->details->setMaximumHeight(104);d_->details->hide();body->addWidget(d_->details);
    connect(d_->detailsToggle,&QPushButton::clicked,this,[this]{d_->details->setVisible(d_->details->isHidden());});
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,this);buttons->button(QDialogButtonBox::Close)->setText("닫기");d_->cancel=buttons->addButton("진행 취소",QDialogButtonBox::ActionRole);d_->cancel->setObjectName("cancelRecordTransfer");body->addWidget(buttons);
    d_->checkpoint=new QTimer(this);d_->checkpoint->setInterval(100);connect(d_->checkpoint,&QTimer::timeout,this,[this]{d_->checkpointExport();});
    connect(d_->prepare,&QPushButton::clicked,this,[this]{QString error;if(!d_->vault.isUnlocked())return;const bool ok=d_->transfer.exists()?d_->transfer.open(&error):d_->transfer.initialize(&error);d_->status->setText(ok?QStringLiteral("기록 이동 준비가 완료되었습니다."):error);if(ok){d_->displayed=d_->transfer.records();d_->page=0;d_->renderRecords();}d_->render();});
    connect(d_->exportSubmit,&QPushButton::clicked,this,[this]{d_->start(true);});connect(d_->importSubmit,&QPushButton::clicked,this,[this]{d_->start(false);});
    connect(d_->cancel,&QPushButton::clicked,this,[this]{d_->cancelOperation();});connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(d_->previous,&QPushButton::clicked,this,[this]{--d_->page;d_->renderRecords();});connect(d_->next,&QPushButton::clicked,this,[this]{++d_->page;d_->renderRecords();});
    connect(&vault,&Vault::aboutToLock,this,[this]{d_->cancelOperation(true);});connect(&vault,&Vault::stateChanged,this,[this]{d_->render();});
    connect(&d_->transfer,&RecordTransfer::locked,this,[this]{d_->displayed={};d_->details->clear();d_->details->hide();d_->detailsToggle->hide();d_->page=0;d_->renderRecords();d_->render();});
    for(auto* control:findChildren<QPushButton*>())control->setMinimumSize(44,44);
    for(auto* control:findChildren<QCheckBox*>()){
        control->setMinimumSize(44,44);control->setProperty("fullRowTarget",true);
    }
    for(auto* control:findChildren<QLineEdit*>())control->setMinimumHeight(44);
    d_->renderRecords();d_->render();
}
RecordTransferDialog::~RecordTransferDialog(){
    // Workers never reference Private. Let their cancellation-aware static codec
    // finish with its own job lifetime; do not delete a running QThread or block UI.
    d_->cancelOperation();if(d_->worker)disconnect(d_->worker,nullptr,this,nullptr);disconnect(&d_->vault,nullptr,this,nullptr);disconnect(&d_->transfer,nullptr,this,nullptr);
}
void RecordTransferDialog::openRecords(){
    if(d_->transfer.exists())d_->openExisting();else d_->status->setText("처음 사용하려면 금고 잠금 해제 후 ‘기록 이동 준비’를 명시적으로 누르세요.");
    show();raise();activateWindow();d_->render();
}
void RecordTransferDialog::done(int result){d_->cancelOperation();QDialog::done(result);}
}
