#include "keefi/PlainToolTip.h"
#include "keefi/InfoButton.h"
#include "keefi/CompactControls.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiVisuals.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPointer>
#include <QVBoxLayout>
#include <QTextDocument>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
namespace keefi {
struct InfoButton::Private {
    InfoButton* q=nullptr;QString title,publicText,privateText;bool privateAllowed=false;
    QPointer<QDialog> dialog;QPointer<QPlainTextEdit> publicWidget,privateWidget;QPointer<QLabel> privateHeading;
    std::optional<PublicSourceInformation> publicSource;
    std::array<QPointer<QLabel>,5> publicSourceValues;
    QPointer<QLabel> publicHeading;
    void render() {
        if(!dialog)return;
        const QPointer<InfoButton> alive(q);const auto currentDialog=dialog;const auto publicBody=publicWidget,privateBody=privateWidget;const auto heading=privateHeading;
        const auto publicCopy=publicText,privateCopy=privateAllowed?privateText:QString{};
        const auto titleCopy=title.isEmpty()?InfoButton::tr("정보"):title;
        const auto sourceCopy=publicSource;
        const auto values=publicSourceValues;
        const auto titleHeading=publicHeading;
        if(currentDialog)currentDialog->setWindowTitle(titleCopy);if(!alive)return;
        if(titleHeading)titleHeading->setText(titleCopy);if(!alive)return;
        if(sourceCopy){
            const std::array<QString,5> fields{sourceCopy->basis,sourceCopy->source,sourceCopy->delay,sourceCopy->unit,sourceCopy->caution};
            for(size_t i=0;i<fields.size();++i){
                if(values[i]&&values[i]->text()!=fields[i])values[i]->setText(fields[i]);
                if(!alive)return;
            }
        }
        if(publicBody&&publicBody->toPlainText()!=publicCopy)publicBody->setPlainText(publicCopy);if(!alive)return;
        if(sourceCopy&&publicBody)publicBody->setVisible(!publicCopy.isEmpty());if(!alive)return;
        if(privateBody){if(privateBody->toPlainText()!=privateCopy)privateBody->setPlainText(privateCopy);if(!alive)return;privateBody->setVisible(!privateCopy.isEmpty());}if(!alive)return;
        if(heading)heading->setVisible(!privateCopy.isEmpty());
    }
};
InfoButton::InfoButton(QWidget* parent) : QToolButton(parent), d_(std::make_unique<Private>()) {
    d_->q=this;setObjectName("infoButton");setText("ⓘ");setAutoRaise(true);setFocusPolicy(Qt::StrongFocus);setMinimumSize(32,32);
    setAccessibleName(tr("정보 보기"));setAccessibleDescription(tr("정의와 원천 정보"));connect(this,&QToolButton::clicked,this,&InfoButton::openInformation);
}
InfoButton::~InfoButton() {
    // Disconnect while Private still exists, before QObject destroys child dialogs.
    const auto dialogs=findChildren<QDialog*>("infoDialog",Qt::FindDirectChildrenOnly);
    for(auto* dialog:dialogs){disconnect(dialog,nullptr,this,nullptr);delete dialog;}
}
void InfoButton::setInformation(const QString& title,const QString& publicText) {
    d_->title=title;d_->publicText=publicText;setAccessibleName(title.isEmpty()?tr("정보 보기"):title+tr(" 정보 보기"));setToolTip(plainToolTip(accessibleName()));d_->render();
}
void InfoButton::setPublicSourceInformation(const QString& title,const QString& summary,
    const PublicSourceInformation& fields,const QString& supplementalText) {
    if(!d_->publicSource){
        applyCompactButton(this,true);
        setToolButtonStyle(Qt::ToolButtonIconOnly);
        setIconSize(QSize(18,18));
        bindThemePresentation(this,[this]{setIcon(informationGlyph(themeColors().text));});
    }
    d_->publicSource=fields;
    d_->title=title;
    d_->publicText=supplementalText;
    const auto name=title.isEmpty()?tr("정보 보기"):title+tr(" 정보 보기");
    if(accessibleName()!=name)setAccessibleName(name);
    const auto tooltip=plainToolTip(summary);
    if(toolTip()!=tooltip)setToolTip(tooltip);
    d_->render();
}
void InfoButton::setPrivateAccess(bool allowed) {if(!allowed){clearPrivate();return;}d_->privateAllowed=true;}
bool InfoButton::setPrivateInformation(const QString& text) {if(!d_->privateAllowed)return false;if(d_->privateText==text)return true;d_->privateText=text;d_->render();return true;}
void InfoButton::clearPrivate() {
    d_->privateAllowed=false;d_->privateText.clear();const QPointer<InfoButton> alive(this);const auto dialog=d_->dialog;
    if(d_->privateWidget)d_->privateWidget->clear();if(!alive)return;
    if(d_->privateHeading)d_->privateHeading->hide();if(dialog)dialog->close();
}
bool InfoButton::informationOpen() const { return d_->dialog&&d_->dialog->isVisible(); }
void InfoButton::openInformation() {
    if(d_->dialog){d_->dialog->raise();d_->dialog->activateWindow();return;}
    auto* dialog=new QDialog(this);d_->dialog=dialog;dialog->setObjectName("infoDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout=new QVBoxLayout(dialog);
    auto* sourceTable=d_->publicSource?new QWidget(dialog):nullptr;
    if(sourceTable){
        auto* heading=new QLabel(dialog);
        heading->setTextFormat(Qt::PlainText);
        heading->setFont(KeefiFonts::font(FontRole::Title17));
        d_->publicHeading=heading;
        layout->addWidget(heading);
        sourceTable->setObjectName("infoPublicSourceTable");
        auto* table=new QGridLayout(sourceTable);
        table->setContentsMargins(0,0,0,0);
        table->setColumnStretch(1,1);
        table->setVerticalSpacing(8);
        const QStringList names{tr("기준"),tr("출처"),tr("지연"),tr("단위"),tr("주의")};
        for(qsizetype i=0;i<names.size();++i){
            auto* name=new QLabel(names[i],sourceTable);
            name->setFont(KeefiFonts::font(FontRole::CaptionStrong12));
            setThemeStyle(name,"color:${text2};");
            auto* value=new QLabel(sourceTable);
            value->setFont(KeefiFonts::font(FontRole::Body15));
            value->setTextFormat(Qt::PlainText);
            value->setWordWrap(true);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            table->addWidget(name,int(i),0,Qt::AlignTop);
            table->addWidget(value,int(i),1);
            d_->publicSourceValues[size_t(i)]=value;
        }
        layout->addWidget(sourceTable);
    }
    auto* publicBody=new QPlainTextEdit(dialog);d_->publicWidget=publicBody;publicBody->setObjectName("infoPublicText");publicBody->setReadOnly(true);publicBody->setAccessibleName(tr("공개 정의와 원천"));layout->addWidget(publicBody,1);
    auto* privateHeading=new QLabel(tr("내 연동"),dialog);d_->privateHeading=privateHeading;privateHeading->setTextFormat(Qt::PlainText);layout->addWidget(privateHeading);
    auto* privateBody=new QPlainTextEdit(dialog);d_->privateWidget=privateBody;privateBody->setObjectName("infoPrivateText");privateBody->setReadOnly(true);privateBody->setAccessibleName(tr("현재 내 연동 정보"));privateBody->setMaximumHeight(140);layout->addWidget(privateBody);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,dialog);
    if(sourceTable){
        auto* close=buttons->button(QDialogButtonBox::Close);
        close->setText(tr("Close"));
        applyCompactButton(close);
        publicBody->setFont(KeefiFonts::font(FontRole::Body15));
    }
    layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    const QPointer<InfoButton> self(this);
    connect(dialog,&QDialog::finished,this,[self,dialog] {
        if(!self||self->d_->dialog!=dialog)return;
        // A closed dialog can await deferred deletion during a subsequent vault lock.
        // Clear its private child synchronously so that the pending widget has no copy.
        const auto privateBody=self->d_->privateWidget;if(privateBody)privateBody->clear();if(!self)return;
        self->d_->dialog.clear();self->d_->publicWidget.clear();self->d_->privateWidget.clear();self->d_->privateHeading.clear();
        self->d_->publicHeading.clear();self->d_->publicSourceValues={};
        if(self->isVisible()){self->window()->activateWindow();if(!self)return;self->setFocus(Qt::OtherFocusReason);}if(self)emit self->informationClosed();
    });
    d_->render();if(!self)return;
    if(sourceTable){
        const int width=std::min(420,std::max(280,window()->width()-32));
        const auto margins=layout->contentsMargins();
        const int tableWidth=width-margins.left()-margins.right();
        int height=margins.top()+margins.bottom()+d_->publicHeading->sizeHint().height()
            +sourceTable->heightForWidth(tableWidth)+buttons->sizeHint().height()+2*layout->spacing();
        if(!publicBody->isHidden()){
            QTextDocument notes;
            notes.setDefaultFont(publicBody->font());
            notes.setPlainText(d_->publicText);
            notes.setTextWidth(tableWidth-12);
            height+=std::max(publicBody->minimumSizeHint().height(),int(std::ceil(notes.size().height()))+4)+layout->spacing();
        }
        dialog->setMaximumHeight(480);
        dialog->resize(width,std::min(480,height));
    }else{
        dialog->resize(std::min(520,std::max(280,window()->width()-32)),380);
    }
    dialog->open();if(self)emit self->informationOpened();
}
void InfoButton::keyPressEvent(QKeyEvent* event) {
    if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){event->accept();if(!event->isAutoRepeat())openInformation();return;}
    QToolButton::keyPressEvent(event);
}
}
