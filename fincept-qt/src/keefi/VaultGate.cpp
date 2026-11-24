#include <QSignalBlocker>
#include "keefi/VaultGate.h"
#include "keefi/CompactControls.h"
#include "keefi/SegmentedControlStyle.h"
#include "keefi/VaultDialog.h"
#include "keefi/VaultMark.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QButtonGroup>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtMath>

namespace keefi {
namespace {
constexpr int ColumnWidth = 340;
QLabel* label(const QString& text, const char* name, FontRole role, QWidget* parent) {
    auto* result=new QLabel(text,parent);result->setObjectName(name);
    result->setTextFormat(Qt::PlainText);result->setWordWrap(true);
    result->setFont(KeefiFonts::font(role));return result;
}
// Secondary routes are quiet text buttons that still show keyboard focus.
QPushButton* link(const QString& text, const char* name, QWidget* parent) {
    auto* result=new QPushButton(text,parent);result->setObjectName(name);result->setProperty("gateLink",true);
    result->setFixedHeight(CompactButtonHeight);result->setFont(KeefiFonts::font(FontRole::Secondary14));
    result->setFocusPolicy(Qt::StrongFocus);result->setCursor(Qt::PointingHandCursor);return result;
}
}

VaultGate::VaultGate(bool exists,const QDateTime&,QWidget* parent)
    : QWidget(parent),exists_(exists) {
    setObjectName("vaultGate");setFont(KeefiFonts::font(FontRole::Body15));
    auto* surround=new QHBoxLayout(this);surround->setContentsMargins(0,0,0,0);surround->setSpacing(0);
    auto* shell=new QWidget(this);shell->setObjectName("vaultGateShell");shell->setMaximumWidth(1280);
    surround->addStretch();surround->addWidget(shell,1);surround->addStretch();
    auto* root=new QVBoxLayout(shell);root->setContentsMargins(40,22,40,26);root->setSpacing(0);
    // Nothing above the form but the theme choice: the wordmark is the form's own figure.
    auto* header=new QHBoxLayout;header->addStretch();
    auto* themeBox=new QWidget(this);themeBox->setObjectName("vaultGateTheme");
    auto* theme=new QHBoxLayout(themeBox);theme->setContentsMargins(0,0,0,0);theme->setSpacing(2);
    auto* light=new QPushButton(tr("흰"),themeBox);auto* dark=new QPushButton(tr("검"),themeBox);
    auto* themeChoice=new QButtonGroup(this);themeChoice->setExclusive(true);
    light->setObjectName("vaultGateLight");dark->setObjectName("vaultGateDark");
    for(auto* button:{light,dark}){button->setCheckable(true);button->setAutoExclusive(true);applySegmentedControlStyle(button, CompactButtonHeight);themeChoice->addButton(button);theme->addWidget(button);}
    connect(light,&QPushButton::toggled,this,[](bool selected){if(selected)KeefiTheme::instance()->setMode(ThemeMode::Light);});
    connect(dark,&QPushButton::toggled,this,[](bool selected){if(selected)KeefiTheme::instance()->setMode(ThemeMode::Dark);});
    // Preserve explicit mode selection even when System already displays this color.
    connect(light,&QPushButton::clicked,this,[]{KeefiTheme::instance()->setMode(ThemeMode::Light);});
    connect(dark,&QPushButton::clicked,this,[]{KeefiTheme::instance()->setMode(ThemeMode::Dark);});
    bindThemePresentation(this,[light,dark]{const QSignalBlocker lightBlock(light),darkBlock(dark);light->setChecked(!KeefiTheme::instance()->isDark());dark->setChecked(KeefiTheme::instance()->isDark());});
    header->addWidget(themeBox);root->addLayout(header);
    scroll_=new QScrollArea(this);scroll_->setObjectName("vaultGateScroll");scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);root->addWidget(scroll_,1);
    auto* body=new QWidget(scroll_);body->setObjectName("vaultGateBody");scroll_->setWidget(body);
    auto* bodyLayout=new QVBoxLayout(body);bodyLayout->setContentsMargins(0,16,0,16);bodyLayout->setSpacing(0);bodyLayout->addStretch();
    // One centred column, the same at every window width.
    auto* row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->addStretch();
    auto* column=new QWidget(body);column->setObjectName("vaultGateColumn");column->setMaximumWidth(ColumnWidth);
    column->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);row->addWidget(column,1);row->addStretch();
    bodyLayout->addLayout(row);bodyLayout->addStretch();
    auto* columnLayout=new QVBoxLayout(column);columnLayout->setContentsMargins(0,0,0,0);columnLayout->setSpacing(0);
    mark_=new VaultMark(column);mark_->setObjectName("vaultGateBrand");columnLayout->addWidget(mark_,0,Qt::AlignHCenter);
    columnLayout->addSpacing(28);
    title_=label({},"vaultGateTitle",FontRole::Title17,column);title_->setAlignment(Qt::AlignHCenter);columnLayout->addWidget(title_);
    columnLayout->addSpacing(18);
    safety_=label({},"vaultGateSafety",FontRole::Secondary14,column);safety_->setAlignment(Qt::AlignHCenter);columnLayout->addWidget(safety_);
    forms_=new QStackedWidget(column);forms_->setObjectName("vaultGateForms");
    create_=new VaultDialog(VaultDialog::Mode::Create,forms_,VaultDialog::Presentation::Embedded);
    unlock_=new VaultDialog(VaultDialog::Mode::Unlock,forms_,VaultDialog::Presentation::Embedded);
    restore_=new VaultDialog(VaultDialog::Mode::Unlock,forms_,VaultDialog::Presentation::Embedded);
    create_->setObjectName("vaultGateCreateForm");unlock_->setObjectName("vaultGateUnlockForm");restore_->setObjectName("vaultGateRestoreForm");
    for(auto* form:{create_,unlock_,restore_}){
        forms_->addWidget(form);
        // The wordmark follows the visible form only, and only through rule flags.
        connect(form,&VaultDialog::ruleProgressChanged,this,[this,form](int rules,bool ready){if(form==forms_->currentWidget())mark_->setProgress(rules,ready);});
        connect(form,&VaultDialog::submitted,this,[this,form](const QString& current,const QString& next){
            busy_=true;refreshControls();
            // The password check blocks this thread next: settle the wordmark first.
            mark_->hold();
            emit submitted(form==create_?int(VaultDialog::Mode::Create):int(VaultDialog::Mode::Unlock),current,next,form==restore_?backupPath_:QString{});
        });
    }
    columnLayout->addWidget(forms_);
    // Secondary routes under the form, centred on it.
    auto* footer=new QWidget(this);footer->setObjectName("vaultGateFooter");
    auto* footerLayout=new QHBoxLayout(footer);footerLayout->setContentsMargins(0,12,0,0);footerLayout->setSpacing(16);footerLayout->addStretch();
    backup_=link(tr("백업에서 복원"),"vaultGateBackup",footer);footerLayout->addWidget(backup_);
    cancelRestore_=link(tr("취소"),"vaultGateRestoreCancel",footer);footerLayout->addWidget(cancelRestore_);
    browse_=link(tr("금고 없이 둘러보기"),"vaultGateBrowse",footer);
    browse_->setAccessibleName(tr("금고 없이 둘러보기 →"));browse_->setLayoutDirection(Qt::RightToLeft);browse_->setIconSize(QSize(18,18));
    bindThemePresentation(browse_,[this]{
        const qreal dpr=browse_->devicePixelRatioF();QPixmap arrow(QSize(qCeil(18*dpr),qCeil(18*dpr)));arrow.setDevicePixelRatio(dpr);arrow.fill(Qt::transparent);
        QPainter painter(&arrow);painter.setFont(KeefiFonts::font(FontRole::Secondary14));painter.setPen(themeColors().accentText);
        painter.drawText(QRect(0,0,18,18),Qt::AlignCenter,QStringLiteral("→"));painter.end();browse_->setIcon(QIcon(arrow));
    });
    footerLayout->addWidget(browse_);footerLayout->addStretch();
    root->addWidget(footer);
    connect(backup_,&QPushButton::clicked,this,&VaultGate::chooseBackup);
    connect(cancelRestore_,&QPushButton::clicked,this,[this]{selectPage(exists_);});
    connect(browse_,&QPushButton::clicked,this,[this]{clearSensitiveInputs();emit browseRequested();});
    setThemeStyle(this,QStringLiteral(R"(
        QWidget#vaultGate { background:${bg}; color:${text}; }
        QWidget#vaultGateTheme { border:1px solid ${rule}; border-radius:8px; }
        QLabel#vaultGateSafety { color:${danger}; }
        QLabel[tone="error"] { color:${danger}; }
        QPushButton#submitVault { background:${text}; color:${bg}; border:0; border-radius:6px; font-weight:600; }
        QPushButton#submitVault:disabled { background:transparent; color:${text3}; border:1px dashed ${field}; }
        QPushButton[gateLink="true"] { background:transparent; color:${text2}; border:2px solid transparent; border-radius:6px; padding:0 6px; }
        QPushButton[gateLink="true"]:hover { color:${text}; }
        QPushButton[gateLink="true"]:focus { border-color:${accent}; }
        QPushButton[gateLink="true"]:disabled { color:${text3}; }
        QLineEdit { background:${input}; border-color:${field}; }
    )"));
    selectPage(exists);
}

void VaultGate::refreshMetadata(bool exists,const QDateTime&){
    exists_=exists;
    // Without tabs the form must follow the file: a vault that appears or
    // disappears switches the page, unless a check is running or a restore is
    // showing its outcome.
    if(!busy_&&forms_->currentWidget()!=restore_&&(forms_->currentWidget()==unlock_)!=exists_)selectPage(exists_);
    else refreshControls();
}
void VaultGate::setSecurityReady(bool ready){ready_=ready;if(!ready)resetPresentation();refreshControls();}
// Authentication failure also emits aboutToLock: clear password bytes there
// without replacing the form that must display the error and allow retry.
void VaultGate::clearSensitiveInputs(){for(auto* form:{create_,unlock_,restore_})form->clearSensitiveInputs();if(chooser_)chooser_->reject();}
void VaultGate::resetPresentation(){clearSensitiveInputs();busy_=false;selectPage(exists_);}
void VaultGate::setOperationError(const QString& error){
    busy_=false;refreshControls();static_cast<VaultDialog*>(forms_->currentWidget())->setError(error);mark_->reject();
}
void VaultGate::showForm(VaultDialog* form){
    // A stacked widget is as tall as its tallest page; size it to the visible
    // form so the column stays centred.
    forms_->setCurrentWidget(form);
    for(int index=0;index<forms_->count();++index){
        auto* page=forms_->widget(index);
        page->setSizePolicy(QSizePolicy::Preferred,page==form?QSizePolicy::Preferred:QSizePolicy::Ignored);
    }
    forms_->updateGeometry();
    mark_->reset();
}
void VaultGate::selectPage(bool existing){
    if(busy_)return;
    for(auto* form:{create_,unlock_,restore_})form->resetForm();backupPath_.clear();
    showForm(existing?unlock_:create_);refreshControls();
}
void VaultGate::refreshControls(){
    const bool restore=forms_->currentWidget()==restore_,unlock=forms_->currentWidget()==unlock_;
    title_->setText(restore?tr("백업에서 복원"):unlock?tr("금고 열기"):tr("새 금고 만들기"));
    safety_->setText(!ready_?tr("PC 잠금 보호를 준비한 뒤 금고를 사용할 수 있어요."):QString{});
    safety_->setVisible(!safety_->text().isEmpty());
    create_->setEnabled(ready_&&!exists_&&!busy_);unlock_->setEnabled(ready_&&exists_&&!busy_);restore_->setEnabled(ready_&&!exists_&&!busy_);
    backup_->setVisible(!unlock&&!restore);backup_->setEnabled(ready_&&!exists_&&!busy_);
    cancelRestore_->setVisible(restore);cancelRestore_->setEnabled(!busy_);
    browse_->setEnabled(!busy_);
    // Rule names belong to a new password only.
    mark_->setCaptionsVisible(!unlock&&!restore);
    mark_->setEnabled(ready_&&(unlock?exists_:!exists_||restore));
}
void VaultGate::chooseBackup(){
    if(!ready_||exists_||busy_||chooser_)return;
    chooser_=new QFileDialog(this,tr("백업한 금고 파일 불러오기"));chooser_->setObjectName("vaultBackupChooser");
    chooser_->setFileMode(QFileDialog::ExistingFile);chooser_->setAcceptMode(QFileDialog::AcceptOpen);
    chooser_->setAttribute(Qt::WA_DeleteOnClose);
    connect(chooser_,&QFileDialog::fileSelected,this,&VaultGate::selectBackup);chooser_->open();
}
void VaultGate::selectBackup(const QString& path){
    if(!ready_||exists_||busy_||path.isEmpty())return;
    for(auto* form:{create_,unlock_,restore_})form->resetForm();
    backupPath_=path;showForm(restore_);refreshControls();
    scroll_->ensureWidgetVisible(restore_);
}
void VaultGate::hideEvent(QHideEvent* event){resetPresentation();QWidget::hideEvent(event);}
}
