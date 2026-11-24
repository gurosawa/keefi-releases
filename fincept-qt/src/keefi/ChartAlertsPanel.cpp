#include <QToolTip>
#include "keefi/ChartAlertsPanel.h"
#include "keefi/PublicFormatMigration.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/KeefiTheme.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartAlertFeed.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/LedgerTextHighlighter.h"
#include "keefi/LocalTableWidget.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QMetaMethod>
#include <QShowEvent>
#include <functional>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QTextEdit>
#include <QTextLayout>
#include <QTimeEdit>
#include <QTimeZone>
#include <QVBoxLayout>
#include <algorithm>
namespace keefi {
namespace {
class AlertCheckBox final : public QCheckBox {
  public:
    using QCheckBox::QCheckBox;
  protected:
    // The 44px target includes the padded row, not just the indicator and ink.
    bool hitButton(const QPoint& position)const override{return rect().contains(position);}
};
class AlertTimeframeCombo final : public QComboBox {
  public:
    explicit AlertTimeframeCombo(QWidget* parent):QComboBox(parent){setItemDelegate(new LedgerItemDelegate(this));}
  protected:
    void paintEvent(QPaintEvent* event)override{
        if(isEditable()){QComboBox::paintEvent(event);return;}
        QStyleOptionComboBox option;initStyleOption(&option);const auto original=option.currentText;option.currentText.clear();
        QPainter painter(this);style()->drawComplexControl(QStyle::CC_ComboBox,&option,&painter,this);
        style()->drawControl(QStyle::CE_ComboBoxLabel,&option,&painter,this);
        const auto area=style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,this);
        painter.setPen(option.palette.color(option.state.testFlag(QStyle::State_Enabled)?QPalette::Active:QPalette::Disabled,QPalette::ButtonText));
        const auto body=font();drawLedgerText(painter,area,original,FontRole::Body15,QStyle::visualAlignment(option.direction,Qt::AlignLeft),&body);
    }
};
class AlertMuteButton final : public QPushButton {
  public:
    using QPushButton::QPushButton;
  protected:
    void paintEvent(QPaintEvent*)override{
        QStyleOptionButton option;initStyleOption(&option);const auto original=option.text;option.text.clear();
        QPainter painter(this);style()->drawControl(QStyle::CE_PushButton,&option,&painter,this);
        auto area=style()->subElementRect(QStyle::SE_PushButtonContents,&option,this);
        if(option.state.testFlag(QStyle::State_Sunken)||option.state.testFlag(QStyle::State_On))
            area.translate(style()->pixelMetric(QStyle::PM_ButtonShiftHorizontal,&option,this),style()->pixelMetric(QStyle::PM_ButtonShiftVertical,&option,this));
        painter.setPen(option.palette.color(option.state.testFlag(QStyle::State_Enabled)?QPalette::Active:QPalette::Disabled,QPalette::ButtonText));
        // QPushButton keeps its complete action label. Preserve that native
        // contract and its sizeHint; table-row elision is not a button action.
        QTextLayout layout(original,font());layout.setFormats(KeefiFonts::numberRuns(original));
        QTextOption textOption;textOption.setWrapMode(QTextOption::NoWrap);layout.setTextOption(textOption);
        layout.beginLayout();auto textLine=layout.createLine();if(textLine.isValid())textLine.setLineWidth(1000000);layout.endLayout();
        if(textLine.isValid()){painter.setClipRect(rect(),Qt::IntersectClip);const auto center=QRectF(area).center();
            layout.draw(&painter,QPointF(center.x()-textLine.naturalTextWidth()/2,center.y()-textLine.height()/2));}
    }
};
class AlertSettingsPage final : public QWidget {
  public:
    using QWidget::QWidget;
    std::function<void()> firstShow;
  protected:
    void showEvent(QShowEvent* event)override{
        QWidget::showEvent(event);
        if(firstShow)firstShow();
    }
};
class AlertHistoryDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const override {
        auto item=option;initStyleOption(&item,index);
        // Qt's style option normalizes newlines to line separators for native
        // painting. Shape the original model paragraphs without losing line 2.
        const auto text=index.data(Qt::DisplayRole).toString();
        const auto* style=item.widget?item.widget->style():QApplication::style();
        const auto area=style->subElementRect(QStyle::SE_ItemViewItemText,&item,item.widget);item.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem,&item,painter,item.widget);
        const auto lines=text.split(QLatin1Char('\n'));const qreal lineHeight=qreal(area.height())/qMax(1,lines.size());
        painter->save();painter->setPen(option.state.testFlag(QStyle::State_Selected)?item.palette.color(QPalette::HighlightedText):index.data(Qt::ForegroundRole).value<QBrush>().color());
        for(int i=0;i<lines.size();++i)drawLedgerText(*painter,QRectF(area.left(),area.top()+i*lineHeight,area.width(),lineHeight),lines[i],FontRole::Body15);
        painter->restore();
    }
};
}
struct ChartAlertsPanel::Private {
    ChartAlertsPanel* q;QPointer<ChartAlertEngine> engine;QPointer<ChartAlertFeed> feed;QPointer<SystemNotificationSink> sink;QPointer<ChartPhrasePack> pack;
    QPointer<QDialog> dialog,review;QLabel* status=nullptr;QCheckBox* drip=nullptr;QListWidget* history=nullptr;
    QList<QPointer<QWidget>> pages;
    QMap<QString,QString> labels;bool background=true,backgroundAvailable=true,closeSeen=false,introSeen=false;
    QByteArray imported,reviewedSha;
    struct Entry {QString id,title,body;bool privateData;MovementDirection direction=MovementDirection::Unknown;bool fixedRisk=false;};QList<Entry> entries;
    bool hasVisibleView()const{
        if(q->isVisible())return true;
        for(const auto& page:pages)if(page&&page->isVisible())return true;
        return false;
    }
    void refreshBackground(){
        for(const auto& page:pages)if(page){
            if(auto* state=page->findChild<QLabel*>("chartAlertBackgroundStatus"))
                state->setText(!backgroundAvailable?QObject::tr("이 환경에서는 메뉴 막대를 사용할 수 없어 창을 닫으면 앱이 종료돼요."):
                    background?QObject::tr("창을 닫아도 메뉴 막대에서 실행합니다."):QObject::tr("창을 닫으면 앱이 종료됩니다."));
        }
    }
    void renderPageHistory(){
        pages.erase(std::remove_if(pages.begin(),pages.end(),[](const auto& page){return page.isNull();}),pages.end());
        for(const auto& page:pages)if(page){
            if(auto* empty=page->findChild<QLabel*>("chartAlertHistoryEmpty"))empty->setVisible(entries.isEmpty());
            for(int i=0;i<30;++i){
                auto* row=page->findChild<QLabel*>("chartAlertHistoryRow_"+QString::number(i));if(!row)continue;
                if(i>=entries.size()){
                    row->clear();row->setAccessibleName({});row->setToolTip({});row->setStyleSheet({});
                    row->setProperty("noticeId",QVariant{});row->setProperty("movementDirection",QVariant{});row->setProperty("fixedRisk",QVariant{});row->hide();continue;
                }
                const auto& entry=entries[entries.size()-1-i];
                const auto text=QObject::tr("시스템 등록 · %1\n%2").arg(movementDecoratedText(entry.title,entry.direction),entry.body);
                row->setText(text);row->setAccessibleName(text);row->setToolTip(QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped().replace(QLatin1Char('\n'),QStringLiteral("<br>"))));
                const auto color=entry.fixedRisk?themeColors().danger:MovementColorPolicy::colorFor(q,entry.direction);
                row->setStyleSheet(QStringLiteral("QLabel { color: %1; padding: 8px 0; }").arg(color.name()));
                row->setProperty("noticeId",entry.id);row->setProperty("movementDirection",QVariant::fromValue(entry.direction));row->setProperty("fixedRisk",entry.fixedRisk);row->show();
            }
        }
    }
    void renderHistory(){if(q->isVisible()){history->clear();for(const auto& entry:entries){auto* item=new QListWidgetItem(QObject::tr("시스템 등록 · %1\n%2").arg(movementDecoratedText(entry.title,entry.direction),entry.body),history);
        item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(item->text().toHtmlEscaped().replace(QLatin1Char('\n'),QStringLiteral("<br>"))));
        item->setForeground(entry.fixedRisk?themeColors().danger:MovementColorPolicy::colorFor(q,entry.direction));
        item->setData(Qt::UserRole,QVariant::fromValue(entry.direction));item->setData(Qt::UserRole+1,entry.fixedRisk);
    }history->setVisible(!entries.isEmpty());}renderPageHistory();}
    void refresh(){
        if(!q->isVisible())return;
        const QSignalBlocker block(drip);drip->setEnabled(engine);if(engine)drip->setChecked(engine->settings().dripEnabled);
        // Only an actionable state is shown: notifications that cannot reach the user.
        // Collection progress and the active phrase pack stay in the settings dialog.
        const bool blocked=!sink||sink->permission()==SystemNoticePermission::Denied||sink->permission()==SystemNoticePermission::Unknown||sink->permission()==SystemNoticePermission::Unavailable;
        status->setText(blocked?usabilityText(UsabilityText::AlertsPermissionOff):QString{});status->setVisible(blocked);
    }
};
ChartAlertsPanel::ChartAlertsPanel(ChartAlertEngine* engine,ChartAlertFeed* feed,SystemNotificationSink* sink,ChartPhrasePack* pack,QWidget* parent)
    :QWidget(parent),d_(std::make_unique<Private>()){
    setObjectName("chartAlertsPanel");d_->q=this;d_->engine=engine;d_->feed=feed;d_->sink=sink;d_->pack=pack;d_->labels.insert("@holdings",tr("보유 (자동)"));
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(16,12,16,12);layout->setSpacing(8);auto* row=new QHBoxLayout;
    setAttribute(Qt::WA_StyledBackground,true);
    setThemeStyle(this,"QWidget#chartAlertsPanel{background:${bg};border:1px solid ${rule};border-radius:12px;}");
    setMinimumHeight(96);setMaximumHeight(148);
    auto* title=new QLabel(tr("알림"),this);title->setObjectName("chartAlertsTitle");title->setFont(KeefiFonts::font(FontRole::Title17));row->addWidget(title);
    setFont(KeefiFonts::font(FontRole::Body15));
    d_->status=new LedgerTextLabel(FontRole::Caption12,this);d_->status->setObjectName("chartAlertStatus");d_->status->setTextFormat(Qt::PlainText);d_->status->setWordWrap(true);row->addWidget(d_->status,1);
    d_->drip=new AlertCheckBox(tr("드립 모드"),this);d_->drip->setObjectName("chartDripEnabled");d_->drip->setChecked(engine?engine->settings().dripEnabled:true);row->addWidget(d_->drip);
    setThemeStyle(d_->drip,"QCheckBox{min-height:44px;min-width:44px;}");
    auto* settings=new QPushButton(tr("알림 설정"),this);settings->setObjectName("chartAlertSettings");row->addWidget(settings);layout->addLayout(row);
    setThemeStyle(settings,"QPushButton{min-height:44px;min-width:44px;padding:0 16px;}");
    d_->history=new QListWidget(this);d_->history->setFont(KeefiFonts::font(FontRole::Body15));d_->history->setItemDelegate(new AlertHistoryDelegate(d_->history));d_->history->setObjectName("chartAlertHistory");d_->history->setAccessibleName(tr("시스템에 등록한 알림 이력 · 실제 표시 여부는 시스템 설정에 따름"));d_->history->setMinimumHeight(0);d_->history->setMaximumHeight(80);d_->history->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);d_->history->hide();layout->addWidget(d_->history);
    connect(settings,&QPushButton::clicked,this,[this]{openSettings();});connect(d_->drip,&QCheckBox::toggled,this,[this](bool enabled){if(d_->engine)d_->engine->setDripEnabled(enabled);});
    if(engine){connect(engine,&ChartAlertEngine::changed,this,[this]{d_->refresh();});connect(engine,&QObject::destroyed,this,[this]{d_->engine=nullptr;d_->refresh();});
        connect(engine,&ChartAlertEngine::noticeSubmitted,this,[this](QString id,QString title,QString body,bool privateData,MovementDirection direction,bool fixedRisk){
            for(const auto& entry:d_->entries)if(entry.id==id)return;d_->entries.append({id,title,body,privateData,direction,fixedRisk});while(d_->entries.size()>30)d_->entries.removeFirst();if(d_->hasVisibleView())d_->renderHistory();});}
    if(feed)connect(feed,&ChartAlertFeed::changed,this,[this]{d_->refresh();});if(sink)connect(sink,&SystemNotificationSink::permissionChanged,this,[this]{d_->refresh();});
    bindMovementPresentation(this,[this]{if(d_->hasVisibleView())d_->renderHistory();});
    bindThemePresentation(this,[this]{if(d_->hasVisibleView())d_->renderHistory();});
}
ChartAlertsPanel::~ChartAlertsPanel(){
    if(d_->engine)disconnect(d_->engine,nullptr,this,nullptr);if(d_->feed)disconnect(d_->feed,nullptr,this,nullptr);if(d_->sink)disconnect(d_->sink,nullptr,this,nullptr);
    if(d_->dialog)delete d_->dialog.data();if(d_->review)delete d_->review.data();for(const auto& page:d_->pages)if(page)delete page.data();d_->entries.clear();
}
void ChartAlertsPanel::setListLabels(const QMap<QString,QString>& labels){
    d_->labels=labels;d_->labels.insert("@holdings",tr("보유 (자동)"));
    for(const auto& page:d_->pages)if(page)if(auto* list=page->findChild<QComboBox*>("chartAlertList")){
        const auto previous=list->currentData().toString();
        {const QSignalBlocker block(list);list->clear();for(auto i=d_->labels.cbegin();i!=d_->labels.cend();++i)list->addItem(i.value(),i.key());
            const auto index=list->findData(previous);list->setCurrentIndex(index>=0?index:list->findData("@holdings"));}
        // Only reload presentation. The selector's signal is not a settings write.
        emit list->currentIndexChanged(list->currentIndex());
    }
}
void ChartAlertsPanel::clearPrivate(){QToolTip::hideText();d_->entries.erase(std::remove_if(d_->entries.begin(),d_->entries.end(),[](const auto& e){return e.privateData;}),d_->entries.end());if(!isVisible())d_->history->clear();d_->renderHistory();}
void ChartAlertsPanel::setBackgroundEnabled(bool value){if(value==d_->background)return;d_->background=value;d_->refreshBackground();emit backgroundEnabledChanged(value);emit publicStateChanged();}
void ChartAlertsPanel::setBackgroundAvailable(bool value){if(value==d_->backgroundAvailable)return;d_->backgroundAvailable=value;d_->refreshBackground();}
bool ChartAlertsPanel::backgroundEnabled()const{return d_->background;}
bool ChartAlertsPanel::closeHintSeen()const{return d_->closeSeen;}
void ChartAlertsPanel::markCloseHintSeen(){if(d_->closeSeen)return;d_->closeSeen=true;emit publicStateChanged();}
QByteArray ChartAlertsPanel::publicState()const{
    return QJsonDocument(QJsonObject{{"format","keefi-chart-alert-ui/1"},{"backgroundEnabled",d_->background},{"closeHintSeen",d_->closeSeen},{"dripIntroSeen",d_->introSeen},
        {"reviewedPack",QString::fromLatin1(d_->imported.toBase64())},{"reviewedSha256",QString::fromLatin1(d_->reviewedSha)}}).toJson(QJsonDocument::Compact);
}
bool ChartAlertsPanel::restorePublicState(const QByteArray& bytes,QString* error){
    const auto fail=[&]{if(error)*error=tr("알림 화면 설정 형식이 올바르지 않습니다.");return false;};
    if(bytes.isEmpty()||bytes.size()>400000)return fail();QJsonParseError issue;const auto doc=QJsonDocument::fromJson(bytes,&issue);if(issue.error!=QJsonParseError::NoError||!doc.isObject())return fail();
    const auto o=doc.object();if(o.size()!=6||canonicalPublicFormat(o.value("format").toString())!="keefi-chart-alert-ui/1"||!o.value("backgroundEnabled").isBool()||!o.value("closeHintSeen").isBool()||!o.value("dripIntroSeen").isBool()||!o.value("reviewedPack").isString()||!o.value("reviewedSha256").isString())return fail();
    const auto encoded=o.value("reviewedPack").toString().toLatin1();const auto imported=QByteArray::fromBase64(encoded,QByteArray::AbortOnBase64DecodingErrors);const auto hash=o.value("reviewedSha256").toString().toLatin1();
    if((!encoded.isEmpty()&&imported.isEmpty())||(imported.isEmpty()!=hash.isEmpty())||(!imported.isEmpty()&&(!d_->pack||!d_->pack->activateReviewed(imported,hash))))return fail();
    if(imported.isEmpty()&&d_->pack)d_->pack->clearImported();d_->imported=imported;d_->reviewedSha=hash;d_->background=o.value("backgroundEnabled").toBool();d_->closeSeen=o.value("closeHintSeen").toBool();d_->introSeen=o.value("dripIntroSeen").toBool();
    d_->refreshBackground();emit backgroundEnabledChanged(d_->background);emit publicStateChanged();if(error)error->clear();d_->refresh();return true;
}
void ChartAlertsPanel::showEvent(QShowEvent* e){QWidget::showEvent(e);d_->renderHistory();d_->refresh();}
void ChartAlertsPanel::hideEvent(QHideEvent* e){
    // Only the legacy standalone view follows this widget's visibility. Embedded
    // option pages belong to their dialog, while this controller remains hidden.
    if(d_->dialog)d_->dialog->close();
    if(d_->review&&d_->review->parentWidget()==this)d_->review->close();
    QWidget::hideEvent(e);
}
QWidget* ChartAlertsPanel::createSettingsPage(QWidget* parent,const QString& requested){
    auto* dialog=new AlertSettingsPage(parent);d_->pages.append(dialog);dialog->setFont(KeefiFonts::font(FontRole::Body15));dialog->setObjectName("chartAlertSettingsPage");
    if(!d_->engine){auto* layout=new QVBoxLayout(dialog);layout->addWidget(new QLabel(tr("알림 연결 없음"),dialog));return dialog;}
    setThemeStyle(dialog,"QPushButton,QCheckBox,QComboBox,QAbstractSpinBox{min-height:44px;min-width:44px;} QPushButton{padding:0 16px;}");
    auto* layout=new QVBoxLayout(dialog);layout->setContentsMargins(0,0,0,0);auto* intro=new LedgerTextLabel(tr("드립 모드가 기본으로 켜져 있어요. 바로 끌 수 있고, 끄면 평범한 기본 문장을 사용합니다. 청산 경고는 항상 진지하게 보냅니다."),FontRole::Secondary14,dialog);
    intro->setObjectName("chartDripIntroduction");intro->setTextFormat(Qt::PlainText);intro->setWordWrap(true);intro->setVisible(!d_->introSeen);layout->addWidget(intro);
    auto* body=new QWidget(dialog);auto* form=new QFormLayout(body);form->setRowWrapPolicy(QFormLayout::WrapLongRows);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);layout->addWidget(body);
    auto* permission=new LedgerTextLabel(d_->sink?d_->sink->status():tr("시스템 알림 연결 없음"),FontRole::Secondary14,body);permission->setTextFormat(Qt::PlainText);permission->setWordWrap(true);form->addRow(tr("시스템 권한"),permission);
    auto* authorize=new QPushButton(tr("시스템 알림 허용"),body);authorize->setObjectName("chartNotificationAuthorize");authorize->setEnabled(d_->sink);form->addRow(authorize);
    connect(authorize,&QPushButton::clicked,dialog,[this]{if(d_->sink)d_->sink->requestAuthorization();});
    if(d_->sink)connect(d_->sink,&SystemNotificationSink::permissionChanged,dialog,[this,permission]{permission->setText(d_->sink?d_->sink->status():tr("시스템 연결 없음"));});
    auto* list=new QComboBox(body);list->setObjectName("chartAlertList");for(auto i=d_->labels.cbegin();i!=d_->labels.cend();++i)list->addItem(i.value(),i.key());form->addRow(tr("대상 리스트"),list);
    auto* enabled=new AlertCheckBox(tr("이 리스트의 알림 켜기"),body);enabled->setObjectName("chartListAlertsEnabled");form->addRow(enabled);
    auto* pullback=new AlertCheckBox(tr("눌림존 첫 진입"),body);auto* results=new AlertCheckBox(tr("평가 결과 · 상승·하락·보합 모두"),body);auto* liquidation=new AlertCheckBox(tr("청산 거리 경고 · 보유만"),body);form->addRow(pullback);form->addRow(results);form->addRow(liquidation);
    auto* whales=new AlertCheckBox(tr("큰손·반전 · 코인 완료 1분"),body);whales->setObjectName("chartAlertWhales");
    whales->setToolTip(tr("기본 꺼짐입니다. 지표 표시와 별개로 켜며 아래 시간봉과 무관하게 실제 완료 1분을 사용합니다. 수집 표본이 부족한 지표는 알리지 않습니다."));form->addRow(whales);
    auto* horizontal=new AlertCheckBox(tr("수평선 닿음 · 그림마다 켠 기준선만"),body);horizontal->setObjectName("chartAlertHorizontal");
    horizontal->setToolTip(tr("공개 리스트에 저장한 수평선에서 따로 켠 알림만 사용합니다. 선마다 고른 시간봉의 실제 완료 봉 두 개로 닿음을 확인하며, 그림 숨김·편집 잠금과는 별개입니다."));form->addRow(horizontal);
    pullback->setObjectName("chartAlertPullback");results->setObjectName("chartAlertResults");liquidation->setObjectName("chartAlertLiquidation");
    auto* unavailable=new LedgerTextLabel(tr("보유 공지: 원천의 구조화 identity가 없어 현재 자동 매칭하지 않습니다."),FontRole::Secondary14,body);unavailable->setTextFormat(Qt::PlainText);unavailable->setWordWrap(true);form->addRow(unavailable);
    auto* timeframe=new AlertTimeframeCombo(body);const QList<qint64> periods{60000,300000,900000,3600000,14400000,86400000};const QStringList periodNames{tr("1분"),tr("5분"),tr("15분"),tr("1시간"),tr("4시간"),tr("1일")};for(int i=0;i<periods.size();++i)timeframe->addItem(periodNames[i],periods[i]);form->addRow(tr("시간봉"),timeframe);
    timeframe->setObjectName("chartAlertTimeframe");
    auto* score=new QSpinBox(body);score->setRange(0,10);form->addRow(tr("최소 표시 점수"),score);auto* cooldown=new QSpinBox(body);cooldown->setRange(0,1440);cooldown->setSuffix(tr("분"));form->addRow(tr("같은 종류 알림 간격"),cooldown);
    auto* distance=new QDoubleSpinBox(body);distance->setRange(.01,100);distance->setDecimals(2);distance->setSuffix("%");form->addRow(tr("청산 거리 기준"),distance);
    score->setFont(KeefiFonts::font(FontRole::Number11));distance->setFont(KeefiFonts::font(FontRole::Number11));
    // The native editable cooldown suffix is Korean; retain its body face and cursor behavior.
    cooldown->setFont(KeefiFonts::font(FontRole::Body15));
    score->setObjectName("chartAlertMinimumScore");cooldown->setObjectName("chartAlertCooldown");distance->setObjectName("chartAlertDistance");
    auto* listHeading=new QLabel(tr("리스트 규칙"),body);listHeading->setProperty("type","sectionHeading");form->insertRow(2,listHeading);
    const auto load=[this,list,enabled,pullback,results,liquidation,whales,horizontal,timeframe,score,cooldown,distance]{
        if(!d_->engine)return;const auto rule=d_->engine->rule(list->currentData().toString());
        const QSignalBlocker a(enabled),b(pullback),c(results),e(liquidation),f(timeframe),g(score),h(cooldown),j(distance),k(whales),l(horizontal);
        enabled->setChecked(rule.enabled);pullback->setChecked(rule.pullback);results->setChecked(rule.results);liquidation->setChecked(rule.liquidation);liquidation->setEnabled(rule.listId=="@holdings");
        whales->setChecked(rule.whales);
        horizontal->setChecked(rule.listId!="@holdings"&&rule.horizontal);horizontal->setEnabled(rule.listId!="@holdings");
        timeframe->setCurrentIndex(timeframe->findData(rule.intervalMs));score->setValue(rule.minimumScore);cooldown->setValue(int(rule.cooldownMs/60000));distance->setValue(rule.liquidationDistancePercent);
    };
    const auto save=[this,list,enabled,pullback,results,liquidation,whales,horizontal,timeframe,score,cooldown,distance]{
        if(!d_->engine)return;auto rule=d_->engine->rule(list->currentData().toString());rule.enabled=enabled->isChecked();rule.pullback=pullback->isChecked();rule.results=results->isChecked();rule.liquidation=rule.listId=="@holdings"&&liquidation->isChecked();rule.whales=whales->isChecked();
        rule.horizontal=rule.listId!="@holdings"&&horizontal->isChecked();rule.intervalMs=timeframe->currentData().toLongLong();rule.minimumScore=score->value();rule.cooldownMs=qint64(cooldown->value())*60000;rule.liquidationDistancePercent=distance->value();d_->engine->setRule(rule);
    };
    const auto requestedIndex=list->findData(requested.isEmpty()?QString("@holdings"):requested);if(requestedIndex>=0)list->setCurrentIndex(requestedIndex);load();
    connect(list,&QComboBox::currentIndexChanged,dialog,load);connect(d_->engine,&ChartAlertEngine::changed,dialog,load);for(auto* box:{enabled,pullback,results,liquidation,whales,horizontal})connect(box,&QCheckBox::toggled,dialog,save);
    connect(timeframe,&QComboBox::currentIndexChanged,dialog,save);connect(score,&QSpinBox::valueChanged,dialog,save);connect(cooldown,&QSpinBox::valueChanged,dialog,save);connect(distance,&QDoubleSpinBox::valueChanged,dialog,save);
    auto* observations=new QLabel(tr("추가 관측 알림 · 기본 꺼짐"),body);observations->setProperty("type","sectionHeading");form->addRow(observations);
    for(const auto& kind:ChartAlertEngine::observationKinds()){
        const auto scope=kind=="rapid_move"?tr(" · 업비트 원화 관측 종목"):tr(" · 선택 차트");
        auto* box=new AlertCheckBox(ChartAlertEngine::observationLabel(kind)+scope,body);
        box->setObjectName("chartObservationAlert_"+kind);box->setChecked(d_->engine->observationEnabled(kind));
        box->setToolTip(tr("새로 확인된 관측부터 알립니다. 자료를 처음 받거나 다시 연결할 때 과거 사건을 알리지 않아요. 조용한 시간과 하루 상한을 함께 적용합니다."));
        form->addRow(box);connect(box,&QCheckBox::toggled,dialog,[this,kind](bool enabled){if(d_->engine)d_->engine->setObservationEnabled(kind,enabled);});
        connect(d_->engine,&ChartAlertEngine::changed,dialog,[this,kind,box]{if(d_->engine){const QSignalBlocker block(box);box->setChecked(d_->engine->observationEnabled(kind));}});
    }
    auto* phraseHeading=new QLabel(tr("알림 문구"),body);phraseHeading->setProperty("type","sectionHeading");form->addRow(phraseHeading);
    const auto settings=d_->engine->settings();auto* drip=new AlertCheckBox(tr("드립 모드 · 끄면 기본 문장"),body);drip->setChecked(settings.dripEnabled);form->addRow(drip);connect(drip,&QCheckBox::toggled,dialog,[this](bool on){if(d_->engine)d_->engine->setDripEnabled(on);});
    drip->setObjectName("chartAlertSettingsDrip");
    connect(d_->engine,&ChartAlertEngine::changed,dialog,[this,drip]{if(d_->engine){const QSignalBlocker block(drip);drip->setChecked(d_->engine->settings().dripEnabled);}});
    auto* quiet=new AlertCheckBox(tr("조용한 시간 사용"),body);quiet->setChecked(settings.quietEnabled);form->addRow(quiet);
    quiet->setObjectName("chartAlertQuietEnabled");
    auto* start=new QTimeEdit(QTime(settings.quietStartMinute/60,settings.quietStartMinute%60),body);auto* end=new QTimeEdit(QTime(settings.quietEndMinute/60,settings.quietEndMinute%60),body);start->setDisplayFormat("HH:mm");end->setDisplayFormat("HH:mm");form->addRow(tr("조용한 시간 시작"),start);form->addRow(tr("종료 · 시작과 같으면 하루 종일"),end);
    auto* zone=new QComboBox(body);for(const auto& id:QList<QByteArray>{settings.timeZoneId,"UTC","Asia/Seoul","America/New_York"})if(zone->findData(id)<0)zone->addItem(QString::fromUtf8(id),id);form->addRow(tr("시간대"),zone);
    auto* cap=new QSpinBox(body);cap->setRange(1,1000);cap->setValue(settings.dailyLimit);form->addRow(tr("하루 상한"),cap);
    start->setFont(KeefiFonts::font(FontRole::Number11));end->setFont(KeefiFonts::font(FontRole::Number11));zone->setFont(KeefiFonts::font(FontRole::Number11));cap->setFont(KeefiFonts::font(FontRole::Number11));
    start->setObjectName("chartAlertQuietStart");end->setObjectName("chartAlertQuietEnd");zone->setObjectName("chartAlertTimeZone");cap->setObjectName("chartAlertDailyLimit");
    const auto saveGlobal=[this,quiet,start,end,zone,cap]{if(!d_->engine)return;auto value=d_->engine->settings();value.quietEnabled=quiet->isChecked();value.quietStartMinute=start->time().hour()*60+start->time().minute();value.quietEndMinute=end->time().hour()*60+end->time().minute();value.timeZoneId=zone->currentData().toByteArray();value.dailyLimit=cap->value();d_->engine->setSettings(value);};
    connect(quiet,&QCheckBox::toggled,dialog,saveGlobal);connect(start,&QTimeEdit::timeChanged,dialog,saveGlobal);connect(end,&QTimeEdit::timeChanged,dialog,saveGlobal);connect(zone,&QComboBox::currentIndexChanged,dialog,saveGlobal);connect(cap,&QSpinBox::valueChanged,dialog,saveGlobal);
    auto* muteHour=new AlertMuteButton(tr("알림 1시간 쉬기"),body);auto* muteToday=new QPushButton(tr("오늘 알림 쉬기"),body);form->addRow(muteHour,muteToday);connect(muteHour,&QPushButton::clicked,dialog,[this]{if(d_->engine)d_->engine->muteForOneHour();});connect(muteToday,&QPushButton::clicked,dialog,[this]{if(d_->engine)d_->engine->muteForToday();});
    auto* backgroundState=new LedgerTextLabel(FontRole::Secondary14,body);backgroundState->setObjectName("chartAlertBackgroundStatus");backgroundState->setWordWrap(true);backgroundState->setTextFormat(Qt::PlainText);form->addRow(tr("백그라운드 실행"),backgroundState);
    auto* general=new QPushButton(tr("일반 옵션에서 변경"),body);general->setObjectName("chartAlertOpenGeneral");form->addRow(general);
    connect(general,&QPushButton::clicked,dialog,[this]{emit generalSettingsRequested();});d_->refreshBackground();
    auto* loadPack=new QPushButton(tr("문구 팩 불러오기 · 검수"),body);auto* plain=new QPushButton(tr("기본 제공 문구로 되돌리기"),body);form->addRow(loadPack,plain);
    loadPack->setObjectName("chartPhraseLoad");plain->setObjectName("chartPhraseUsePlain");
    connect(loadPack,&QPushButton::clicked,dialog,[this,dialog]{const auto path=QFileDialog::getOpenFileName(dialog,tr("문구 팩 선택"),{},tr("JSON (*.json)"));if(path.isEmpty())return;QFile file(path);if(file.open(QIODevice::ReadOnly))reviewPhrasePack(file.read(256*1024+1));});
    connect(plain,&QPushButton::clicked,dialog,[this]{if(d_->pack)d_->pack->clearImported();d_->imported.clear();d_->reviewedSha.clear();emit publicStateChanged();d_->refresh();});
    auto* historyHeading=new QLabel(tr("최근 알림"),body);historyHeading->setProperty("type","sectionHeading");form->addRow(historyHeading);
    auto* explanation=new LedgerTextLabel(tr("최근 30건 · 시스템에 등록한 알림입니다. 실제 표시 여부는 시스템 설정에 따릅니다. 앱을 종료하면 이력은 지워집니다."),FontRole::Secondary14,body);
    explanation->setObjectName("chartAlertHistoryMeaning");explanation->setTextFormat(Qt::PlainText);explanation->setWordWrap(true);form->addRow(explanation);
    auto* history=new QWidget(body);history->setObjectName("chartAlertRecentHistory");auto* rows=new QVBoxLayout(history);rows->setContentsMargins(0,0,0,0);rows->setSpacing(4);
    auto* empty=new QLabel(tr("시스템에 등록한 알림이 아직 없습니다."),history);empty->setObjectName("chartAlertHistoryEmpty");empty->setWordWrap(true);rows->addWidget(empty);
    // Fixed rows avoid a dynamically rebuilt item model in the disposable options
    // dialog. Keep complete, selectable text and a bounded accessibility tree.
    for(int i=0;i<30;++i){
        auto* row=new LedgerTextLabel(FontRole::Body15,history);row->setObjectName("chartAlertHistoryRow_"+QString::number(i));row->setTextFormat(Qt::PlainText);row->setWordWrap(true);
        row->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);row->setMinimumHeight(44);row->hide();rows->addWidget(row);
    }
    form->addRow(history);d_->renderPageHistory();
    dialog->firstShow=[this]{
        d_->renderPageHistory();d_->refreshBackground();
        if(!d_->introSeen){d_->introSeen=true;emit publicStateChanged();}
    };
    return dialog;
}
void ChartAlertsPanel::selectSettingsList(const QString& requested){
    if(requested.isEmpty())return;
    for(const auto& page:d_->pages)if(page)if(auto* selector=page->findChild<QComboBox*>("chartAlertList")){
        const auto index=selector->findData(requested);if(index>=0)selector->setCurrentIndex(index);
        for(auto* ancestor=page->parentWidget();ancestor;ancestor=ancestor->parentWidget())if(auto* scroll=qobject_cast<QScrollArea*>(ancestor)){scroll->ensureWidgetVisible(selector);break;}
    }
}
void ChartAlertsPanel::openSettings(const QString& requested){
    if(!d_->engine)return;
    if(isSignalConnected(QMetaMethod::fromSignal(&ChartAlertsPanel::settingsRequested))){emit settingsRequested(requested);return;}
    if(d_->dialog&&d_->dialog->isVisible()){selectSettingsList(requested);d_->dialog->raise();return;}
    // Standalone embedding remains available to small consumers and test fixtures.
    // The product binds settingsRequested to the single global Options dialog.
    auto* dialog=new QDialog(this);d_->dialog=dialog;dialog->setFont(KeefiFonts::font(FontRole::Body15));dialog->setObjectName("chartAlertSettingsDialog");
    dialog->setWindowTitle(tr("알림 옵션"));dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->resize(560,680);
    setThemeStyle(dialog,"QPushButton,QCheckBox,QComboBox,QAbstractSpinBox{min-height:44px;min-width:44px;} QPushButton{padding:0 16px;}");
    auto* layout=new QVBoxLayout(dialog);auto* scroll=new QScrollArea(dialog);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidgetResizable(true);scroll->setWidget(createSettingsPage(scroll,requested));layout->addWidget(scroll,1);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,dialog);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::close);dialog->open();
}
void ChartAlertsPanel::reviewPhrasePack(const QByteArray& bytes){
    if(d_->review){d_->review->raise();return;}const auto validation=ChartPhrasePack::validate(bytes);
    QWidget* owner=this;for(auto i=d_->pages.crbegin();i!=d_->pages.crend();++i)if(*i&&(*i)->isVisible()){owner=i->data();break;}
    auto* dialog=new QDialog(owner);d_->review=dialog;dialog->setFont(KeefiFonts::font(FontRole::Body15));dialog->setObjectName("chartPhraseReview");dialog->setWindowTitle(tr("문구 팩 검수"));dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->resize(600,560);auto* layout=new QVBoxLayout(dialog);
    auto* description=new LedgerTextLabel(validation.valid?tr("자동 검사는 내용 검수를 대신하지 않습니다. 모든 문장을 읽고 확인한 정확한 팩만 적용합니다."):tr("검사에 실패했습니다. 문구 팩은 적용되지 않습니다."),FontRole::Secondary14,dialog);description->setWordWrap(true);description->setTextFormat(Qt::PlainText);layout->addWidget(description);
    auto* preview=new QTextEdit(dialog);preview->setObjectName("chartPhraseReviewText");preview->setReadOnly(true);QStringList lines;
    if(validation.valid)for(const auto& phrase:validation.preview)lines<<phrase.kind+" · "+phrase.phraseId+"\n"+phrase.text;
    else for(const auto& issue:validation.issues)lines<<issue.code+" · "+issue.kind+" · "+issue.field;
    preview->setFont(KeefiFonts::font(FontRole::Body15));new LedgerTextHighlighter(preview->document());preview->setPlainText(lines.join("\n\n"));layout->addWidget(preview,1);
    auto* reviewed=new QCheckBox(tr("모든 문장을 읽고 내용과 자리표시자를 확인했습니다."),dialog);reviewed->setObjectName("chartPhraseReviewed");reviewed->setEnabled(validation.valid);layout->addWidget(reviewed);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel,dialog);auto* apply=buttons->button(QDialogButtonBox::Apply);apply->setObjectName("chartPhraseApply");apply->setEnabled(false);layout->addWidget(buttons);
    connect(reviewed,&QCheckBox::toggled,dialog,[this,apply,valid=validation.valid](bool checked){apply->setEnabled(checked&&valid&&d_->pack);});
    connect(apply,&QPushButton::clicked,dialog,[this,dialog,bytes,hash=validation.sha256]{if(!d_->pack||!d_->pack->activateReviewed(bytes,hash))return;d_->imported=bytes;d_->reviewedSha=hash;emit publicStateChanged();d_->refresh();dialog->accept();});
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);dialog->open();
}
}
