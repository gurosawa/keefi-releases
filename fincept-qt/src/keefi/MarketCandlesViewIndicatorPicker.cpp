#include "keefi/MarketCandlesView.h"
#include "keefi/MarketCandlesViewFormatting_p.h"
#include "keefi/KeefiTheme.h"
#include "keefi/ProductIdentity.h"
#include "keefi/KeefiFonts.h"
#include "keefi/CompactControls.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/LocalComboBox.h"
#include "keefi/SegmentedControlStyle.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/ChartStudyHelp.h"
#include "keefi/ChartStudySearch.h"
#include "keefi/ChartIndicatorState.h"
#include "keefi/ChartStudies.h"
#include "keefi/ExtendedChartStudies.h"
#include "keefi/MarketCandles.h"
#include "keefi/StockChartTypes.h"

#include <QApplication>
#include <QAccessibleWidget>
#include <QScreen>
#include <QKeySequence>
#include <QSpinBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFocusEvent>
#include <QHeaderView>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyleOptionComboBox>
#include <QShortcut>
#include <QTabBar>
#include <QTreeWidget>
#include <QScrollArea>
#include <QTimer>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QBrush>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QIcon>
#include <QChar>
#include <QList>
#include <QMap>
#include <QModelIndex>
#include <QObject>
#include <QPair>
#include <QPalette>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QWidget>

namespace keefi {
using namespace market_candles_detail;
namespace {
QString studyCategory(const QString& id){
    if(QStringList{"cloud","ma","bb","levels","breakout","trend","relative_strength"}.contains(id))return QStringLiteral("trend");
    if(QStringList{"rsi","macd","adx"}.contains(id))return QStringLiteral("momentum");
    if(QStringList{"volume","cvd","low_volume","stock_flow"}.contains(id))return QStringLiteral("volume");
    if(QStringList{"oi","contract","sentiment"}.contains(id))return QStringLiteral("futures");
    if(id=="similarity")return QStringLiteral("history");
    return QStringLiteral("keefi");
}
// The model keeps CheckStateRole for native accessibility. A separate, explicit
// On/Off column replaces the theme-dependent checkbox glyph in the name cell.
class StudySearchReasonDelegate final:public QStyledItemDelegate {
    static constexpr int NameLinePadding=6;
    static constexpr int ReasonBottomPadding=2;

public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void initStyleOption(QStyleOptionViewItem* option,const QModelIndex& index)const override{
        QStyledItemDelegate::initStyleOption(option,index);
        if(index.column()==0)option->features&=~QStyleOptionViewItem::HasCheckIndicator;
        if(index.column()==5)option->features&=~QStyleOptionViewItem::WrapText;
        if(index.column()==0||index.column()==5){
            const auto foreground=index.data(Qt::ForegroundRole).value<QBrush>();
            if(foreground.style()!=Qt::NoBrush)
                option->palette.setBrush(QPalette::HighlightedText,foreground);
        }
        if(option->state&QStyle::State_Selected)option->palette.setColor(QPalette::Highlight,themeColors().surface);
    }
    QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override{
        QSize size;
        if(index.column()==2){
            QStyleOptionViewItem measured(option);initStyleOption(&measured,index);
            size={measured.fontMetrics.horizontalAdvance(measured.text)+28,CompactButtonHeight};
        }else if(index.column()==5&&!index.data(Qt::DisplayRole).toString().isEmpty()){
            QStyleOptionViewItem measured(option);initStyleOption(&measured,index);
            const auto minimum=measured.fontMetrics.horizontalAdvance(MarketCandlesView::tr("Not calculated"));
            const auto width=qMax(minimum,measured.fontMetrics.horizontalAdvance(measured.text));
            size={width+12,measured.fontMetrics.height()+8};
        }else{
            size=QStyledItemDelegate::sizeHint(option,index);
        }
        const auto name=index.siblingAtColumn(0);
        if(!name.data(Qt::UserRole+1).toString().isEmpty()){
            QStyleOptionViewItem measured(option);initStyleOption(&measured,name);
            const int reasonHeight=QFontMetrics(KeefiFonts::font(FontRole::Caption12)).height();
            // A fixed-height action can reduce QTreeView's running row height.
            // Supply the minimum on every cell so column order cannot clip the reason.
            size.setHeight(qMax(size.height(),measured.fontMetrics.height()+NameLinePadding+reasonHeight+ReasonBottomPadding));
        }
        return size;
    }
    void updateEditorGeometry(QWidget* editor,const QStyleOptionViewItem& option,const QModelIndex& index)const override {
        auto centered=option;
        if(index.column()==1||index.column()==3||index.column()==4||index.column()==6){
            const int height=qMin(option.rect.height(),editor->maximumHeight());
            centered.rect.setY(option.rect.y()+(option.rect.height()-height)/2);
            centered.rect.setHeight(height);
        }
        QStyledItemDelegate::updateEditorGeometry(editor,centered,index);
    }
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override{
        if(index.column()==2&&!index.siblingAtColumn(0).data(Qt::UserRole).toString().isEmpty()){
            QStyleOptionViewItem cell(option);initStyleOption(&cell,index);
            const auto* style=cell.widget?cell.widget->style():QApplication::style();
            const auto label=cell.text;cell.text.clear();
            style->drawControl(QStyle::CE_ItemViewItem,&cell,painter,cell.widget);
            const bool applied=index.siblingAtColumn(0).data(Qt::CheckStateRole).toInt()==Qt::Checked;
            const qreal buttonHeight=CompactButtonHeight-1;
            const qreal top=QRectF(cell.rect).center().y()-buttonHeight/2;
            const QRectF button(cell.rect.left()+4,top,cell.rect.width()-8,buttonHeight);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setFont(cell.font);
            painter->setPen(themeColors().rule);
            painter->setBrush(applied?themeColors().text:(option.state&QStyle::State_MouseOver)?themeColors().surface:themeColors().bg);
            painter->drawRoundedRect(button,6,6);
            painter->setPen(applied?themeColors().bg:themeColors().text);
            painter->drawText(button,Qt::AlignCenter,label);
            painter->restore();
            return;
        }
        const auto reason=index.data(Qt::UserRole+1).toString();
        if(index.column()!=0||reason.isEmpty()){QStyledItemDelegate::paint(painter,option,index);return;}
        QStyleOptionViewItem full(option);initStyleOption(&full,index);
        const auto* style=full.widget?full.widget->style():QApplication::style();
        auto background=full;background.text.clear();background.features&=~QStyleOptionViewItem::HasCheckIndicator;
        style->drawControl(QStyle::CE_ItemViewItem,&background,painter,full.widget);
        auto name=option;name.rect.setHeight(qMin(full.fontMetrics.height()+NameLinePadding,option.rect.height()));name.state&=~QStyle::State_HasFocus;
        QStyledItemDelegate::paint(painter,name,index);
        auto nameLayout=full;nameLayout.rect=name.rect;
        const auto labelRect=style->subElementRect(QStyle::SE_ItemViewItemText,&nameLayout,full.widget);
        const QRect reasonRect(labelRect.left(),name.rect.bottom()+1,qMax(0,option.rect.right()-labelRect.left()-4),
                               option.rect.height()-name.rect.height()-ReasonBottomPadding);
        painter->save();painter->setClipRect(option.rect);
        painter->setFont(KeefiFonts::font(FontRole::Caption12));painter->setPen(themeColors().text3);
        painter->drawText(reasonRect,Qt::AlignLeft|Qt::AlignVCenter,painter->fontMetrics().elidedText(reason,Qt::ElideRight,reasonRect.width()));
        painter->restore();
    }
};
QString studyColorLabel(const QString& key) {
    using View = MarketCandlesView;
    if (key=="bbUpper") return View::tr("Upper band");
    if (key=="bbMiddle") return View::tr("Middle band");
    if (key=="bbLower") return View::tr("Lower band");
    if (key=="macdSignal") return View::tr("Signal line");
    if (key=="macdHistogramUp") return View::tr("Positive histogram");
    if (key=="macdHistogramDown") return View::tr("Negative histogram");
    if (key.endsWith("Up")) return View::tr("Rising");
    if (key.endsWith("Down")) return View::tr("Falling");
    if (key=="similarity") return View::tr("Mean path and range");
    if (key.startsWith("similarity")) return View::tr("Historical path %1").arg(key.right(1));
    if (key=="pullback") return View::tr("Zone");
    return View::tr("Line");
}

class StudyColorButton final : public QPushButton {
  public:
    StudyColorButton(const QString& key, const QString& name, const QColor& color, QWidget* parent)
        : QPushButton(parent), key_(key), label_(name) {
        setObjectName("chartStudyColor_"+key);applyCompactButton(this);
        setColor(color);
        bindThemePresentation(this,[this]{refreshSwatch();});
        connect(this,&QPushButton::clicked,this,[this] {
            QColorDialog picker(effectiveColor(),this);
            picker.setObjectName("chartStudyColorDialog");
            picker.setWindowTitle(label_);
            picker.setOption(QColorDialog::DontUseNativeDialog);
            if (picker.exec()==QDialog::Accepted) setColor(picker.selectedColor());
        });
    }
    void setColor(const QColor& value) {
        color_=value;
        refreshSwatch();
    }
    QColor color() const { return color_; }
  private:
    QColor effectiveColor() const { return color_.isValid()?color_:defaultStudyColor(key_,this); }
    void refreshSwatch() {
        const auto ink=effectiveColor();
        const auto hex=ink.name().toUpper();
        const auto caption=color_.isValid()?hex:MarketCandlesView::tr("Theme");
        // Reserve the whole hexadecimal value before the first palette edit.
        const auto metrics=fontMetrics();
        int digitWidth=0;
        for (const auto digit : QStringLiteral("0123456789ABCDEF"))
            digitWidth=qMax(digitWidth,metrics.horizontalAdvance(digit));
        setIconSize(QSize(14,14));
        setMinimumWidth(qMax(metrics.horizontalAdvance(caption),metrics.horizontalAdvance(QLatin1Char('#'))+6*digitWidth)
                        +iconSize().width()+32);
        setText(caption);setAccessibleName(label_+" · "+(color_.isValid()?hex:caption+" "+hex));
        QPixmap swatch(14,14);swatch.fill(ink);setIcon(QIcon(swatch));
    }
    QString key_,label_;
    QColor color_;
};

void editStudyOptions(ChartStudyPreferences& prefs,const QString& id,const QString& title,QWidget* parent) {
    using View=MarketCandlesView;
    QDialog dialog(parent);dialog.setObjectName("chartStudySettingsDialog_"+id);
    dialog.setWindowTitle(title+" · "+View::tr("Settings"));
    auto* layout=new QVBoxLayout(&dialog);
    auto* form=new QFormLayout;layout->addLayout(form);
    QPointer<ChartStudyPreferences> owner(&prefs);
    QObject::connect(&prefs,&QObject::destroyed,&dialog,&QDialog::reject);
    ChartStudySettings draft=prefs.studySettings();
    const ChartStudySettings defaults;
    struct IntField { QSpinBox* editor; int ChartStudySettings::*member; };
    QList<IntField> fields;
    const auto integer=[&](const QString& key,const QString& name,int ChartStudySettings::*member,int maximum) {
        auto* spin=new QSpinBox(&dialog);spin->setObjectName("chartStudyInput_"+key);
        spin->setRange(2,maximum);spin->setValue(draft.*member);spin->setAccessibleName(name);
        applyCompactInput(spin);form->addRow(name,spin);fields.append({spin,member});return spin;
    };
    QSpinBox *fast=nullptr,*slow=nullptr,*signal=nullptr;
    QDoubleSpinBox* deviations=nullptr;
    if(id=="rsi") integer("rsiPeriod",View::tr("RSI length"),&ChartStudySettings::rsiPeriod,120);
    if(id=="macd"){
        fast=integer("macdFast",View::tr("Fast EMA"),&ChartStudySettings::macdFast,120);
        slow=integer("macdSlow",View::tr("Slow EMA"),&ChartStudySettings::macdSlow,120);
        signal=integer("macdSignal",View::tr("Signal EMA"),&ChartStudySettings::macdSignal,120);
    }
    if(id=="bb"){
        integer("bandPeriod",View::tr("Band length"),&ChartStudySettings::displayBandPeriod,ChartDisplayBandPeriod);
        deviations=new QDoubleSpinBox(&dialog);deviations->setObjectName("chartStudyInput_bandDeviations");
        deviations->setDecimals(2);deviations->setRange(0.1,10.0);deviations->setSingleStep(0.1);
        deviations->setValue(draft.displayBandDeviations);deviations->setAccessibleName(View::tr("Standard deviations"));
        applyCompactInput(deviations);form->addRow(View::tr("Standard deviations"),deviations);
    }
    QMap<QString,StudyColorButton*> colors;
    for(const auto& key:ChartStudyPreferences::colorKeys(id)){
        const auto label=studyColorLabel(key)+" · "+View::tr("Color");
        auto* color=new StudyColorButton(key,label,draft.colors.value(key),&dialog);
        form->addRow(label,color);colors.insert(key,color);
    }
    auto* error=new QLabel(&dialog);error->setWordWrap(true);error->hide();setThemeStyle(error,"color:${danger};");layout->addWidget(error);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel|QDialogButtonBox::RestoreDefaults,&dialog);
    auto* save=buttons->button(QDialogButtonBox::Save);save->setObjectName("chartStudySave");
    auto* reset=buttons->button(QDialogButtonBox::RestoreDefaults);reset->setObjectName("chartStudyReset");
    for(auto* button:buttons->buttons())if(auto* push=qobject_cast<QPushButton*>(button))applyCompactButton(push);
    layout->addWidget(buttons);
    if(fast){
        const auto validate=[=]{
            QString reason;
            if(fast->value()>=slow->value())reason=View::tr("Slow EMA must be longer than Fast EMA.");
            else if(slow->value()+signal->value()>201)reason=View::tr("Slow EMA + Signal EMA must be 201 or less (200 completed candles).");
            error->setText(reason);error->setVisible(!reason.isEmpty());save->setEnabled(reason.isEmpty());
        };
        for(auto* spin:{fast,slow,signal})QObject::connect(spin,&QSpinBox::valueChanged,&dialog,validate);
        validate();
    }
    QObject::connect(reset,&QPushButton::clicked,&dialog,[&]{
        for(const auto& field:fields)field.editor->setValue(defaults.*field.member);
        if(deviations)deviations->setValue(defaults.displayBandDeviations);
        for(auto* button:colors)button->setColor({});
    });
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        if(!save->isEnabled()||!owner)return;
        draft=owner->studySettings();
        for(const auto& field:fields)draft.*field.member=field.editor->value();
        if(deviations)draft.displayBandDeviations=deviations->value();
        for(auto i=colors.cbegin();i!=colors.cend();++i){
            if(i.value()->color().isValid())draft.colors.insert(i.key(),i.value()->color());
            else draft.colors.remove(i.key());
        }
        if(owner->setStudySettings(draft,owner->movingAverages()))dialog.accept();
    });
    dialog.exec();
}

// Name and Apply cells toggle the model. Adjacent information, visibility and
// favorite controls own their input; activating one cannot toggle the row.
class StudyPickerTree final:public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
protected:
    void mousePressEvent(QMouseEvent* event)override{
        auto* item=itemAt(event->position().toPoint());
        const int column=columnAt(int(event->position().x()));
        if(event->button()==Qt::LeftButton&&(column==0||column==2)&&item&&!item->data(0,Qt::UserRole).toString().isEmpty()){
            setCurrentItem(item);setFocus();pressed_=item;event->accept();return;}
        QTreeWidget::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event)override{
        if(pressed_){auto* item=pressed_;pressed_=nullptr;const int column=columnAt(int(event->position().x()));if(event->button()==Qt::LeftButton&&(column==0||column==2)&&itemAt(event->position().toPoint())==item)toggle(item);event->accept();return;}
        QTreeWidget::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent* event)override{
        if((event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter||event->key()==Qt::Key_Space)&&currentItem()&&!currentItem()->data(0,Qt::UserRole).toString().isEmpty()){
            toggle(currentItem());event->accept();return;}QTreeWidget::keyPressEvent(event);
    }
private:
    QTreeWidgetItem* pressed_=nullptr;
    static void toggle(QTreeWidgetItem* item){item->setCheckState(0,item->checkState(0)==Qt::Checked?Qt::Unchecked:Qt::Checked);}
};
class StudySearchEdit final:public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    QTreeWidget* studies=nullptr;
protected:
    void keyPressEvent(QKeyEvent* event)override{
        if(studies&&(event->key()==Qt::Key_Down||event->key()==Qt::Key_Up)){
            studies->setFocus(Qt::TabFocusReason);event->accept();return;
        }
        if(studies&&(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter)){
            if(auto* item=studies->currentItem();item&&!item->isHidden()&&!item->data(0,Qt::UserRole).toString().isEmpty())
                item->setCheckState(0,item->checkState(0)==Qt::Checked?Qt::Unchecked:Qt::Checked);
            event->accept();return;
        }
        QLineEdit::keyPressEvent(event);
    }
};
class FullTargetCheckBox final:public QCheckBox {
public:using QCheckBox::QCheckBox;
protected:bool hitButton(const QPoint& pos)const override{return rect().contains(pos);}
};
// QSpinBox normally replaces an empty editor with the previous value on
// focus loss. Keep that intermediate input visible while the dialog blocks Save.
class StudyLengthSpinBox final : public QSpinBox {
public:
    using QSpinBox::QSpinBox;
    QLineEdit* editor() const { return lineEdit(); }
protected:
    void focusOutEvent(QFocusEvent* event) override {
        if (!editor()->text().isEmpty()) {
            QSpinBox::focusOutEvent(event);
            return;
        }
        const QSignalBlocker block(editor());
        QSpinBox::focusOutEvent(event);
        editor()->clear();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (editor()->text().isEmpty() && (event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter)) {
            event->accept();
            return;
        }
        QSpinBox::keyPressEvent(event);
    }
};
class StudyActionButton final:public QPushButton {
public:
    explicit StudyActionButton(QWidget* parent):QPushButton(parent){setAutoDefault(false);setDefault(false);setFocusPolicy(Qt::StrongFocus);}
protected:
    void keyPressEvent(QKeyEvent* event)override{
        if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){click();event->accept();return;}
        QPushButton::keyPressEvent(event);
    }
};
// Qt's item-view accessibility tree describes model cells, not the QPushButtons
// installed with setItemWidget(). Keep the table interface and keyboard behavior
// intact, and expose these real controls as children of this picker dialog.
constexpr auto StudyPickerAccessibleProperty = "keefiStudyPickerAccessible";
constexpr auto StudyPickerActionProperty = "keefiStudyPickerAction";

class StudyPickerActionAccessible final : public QAccessibleWidget {
public:
    explicit StudyPickerActionAccessible(QPushButton* button)
        : QAccessibleWidget(button, QAccessible::Button) {}

    QAccessibleInterface* parent() const override {
        if (auto* button = qobject_cast<QPushButton*>(object()))
            return QAccessible::queryAccessibleInterface(button->window());
        return nullptr;
    }
    QAccessible::Role role() const override {
        const auto* button = qobject_cast<QPushButton*>(object());
        return button && button->isCheckable() ? QAccessible::CheckBox : QAccessible::Button;
    }
    QRect rect() const override {
        auto bounds = QAccessibleWidget::rect();
        // Item widgets can extend beyond the scrolled viewport. Their accessible
        // hit target must have the same clipping as the visible button.
        if (const auto* button = qobject_cast<QPushButton*>(object())) {
            for (auto* parent = button->parentWidget(); parent; parent = parent->parentWidget()) {
                bounds = bounds.intersected(QRect(parent->mapToGlobal(QPoint()), parent->size()));
                if (parent->isWindow()) break;
            }
        }
        return bounds;
    }
    QAccessible::State state() const override {
        auto result = QAccessibleWidget::state();
        if (const auto* button = qobject_cast<QPushButton*>(object())) {
            result.checkable = button->isCheckable();
            result.checked = button->isChecked();
            result.pressed = button->isDown();
            const bool clipped = button->isVisible() && rect().isEmpty();
            result.offscreen = result.offscreen || clipped;
            result.invisible = result.invisible || clipped;
        }
        return result;
    }
    QStringList actionNames() const override {
        const auto* button = qobject_cast<QPushButton*>(object());
        QStringList actions;
        if (button && button->isEnabled()) {
            if (button->isCheckable()) actions.append(QAccessibleActionInterface::toggleAction());
            actions.append(QAccessibleActionInterface::pressAction());
        }
        actions.append(QAccessibleWidget::actionNames());
        return actions;
    }
    void doAction(const QString& action) override {
        auto* button = qobject_cast<QPushButton*>(object());
        if (!button || !button->isEnabled() || !button->isVisible()) return;
        // Match Qt's button action semantics: return from native AXPress before
        // clicked() can enter the information dialog's modal event loop.
        if (action == QAccessibleActionInterface::pressAction())
            button->animateClick();
        else if (button->isCheckable() && action == QAccessibleActionInterface::toggleAction())
            button->toggle();
        else
            QAccessibleWidget::doAction(action);
    }
};

class StudyPickerDialogAccessible final : public QAccessibleWidget {
public:
    explicit StudyPickerDialogAccessible(QDialog* dialog)
        : QAccessibleWidget(dialog, QAccessible::Dialog) {}

    int childCount() const override {
        return QAccessibleWidget::childCount() + rowActions().size();
    }
    QAccessibleInterface* child(int index) const override {
        const int ordinary = QAccessibleWidget::childCount();
        if (index < ordinary) return QAccessibleWidget::child(index);
        const auto actions = rowActions();
        const int action = index - ordinary;
        return action >= 0 && action < actions.size()
            ? QAccessible::queryAccessibleInterface(actions[action]) : nullptr;
    }
    int indexOfChild(const QAccessibleInterface* child) const override {
        if (!child) return -1;
        const auto actions = rowActions();
        for (int i = 0; i < actions.size(); ++i)
            if (actions[i] == child->object()) return QAccessibleWidget::childCount() + i;
        return QAccessibleWidget::indexOfChild(child);
    }
    QAccessibleInterface* childAt(int x, int y) const override {
        for (auto* action : rowActions()) {
            auto* accessible = QAccessible::queryAccessibleInterface(action);
            if (accessible && accessible->rect().contains(x, y)) return accessible;
        }
        return QAccessibleWidget::childAt(x, y);
    }
private:
    QList<QPushButton*> rowActions() const {
        QList<QPushButton*> actions;
        if (auto* dialog = widget()) {
            for (auto* button : dialog->findChildren<QPushButton*>())
                if (button->property(StudyPickerActionProperty).toBool() && button->isVisibleTo(dialog))
                    actions.append(button);
        }
        return actions;
    }
};

QAccessibleInterface* studyPickerAccessibleFactory(const QString&, QObject* object) {
    if (object->property(StudyPickerAccessibleProperty).toBool()) {
        if (auto* dialog = qobject_cast<QDialog*>(object)) return new StudyPickerDialogAccessible(dialog);
    }
    if (object->property(StudyPickerActionProperty).toBool()) {
        if (auto* button = qobject_cast<QPushButton*>(object)) return new StudyPickerActionAccessible(button);
    }
    return nullptr;
}

void ensureStudyPickerAccessibility() {
    static const bool installed = [] { QAccessible::installFactory(studyPickerAccessibleFactory); return true; }();
    Q_UNUSED(installed);
}

class StudyCategoryCombo final:public LocalComboBox {
public:
    explicit StudyCategoryCombo(QWidget* parent):LocalComboBox(parent){
        // Keep the existing clearance checked at larger accessibility fonts.
        setCompact(5);
        setMinimumWidth(200);
        setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Fixed);
    }
    QSize sizeHint()const override{return measuredSize();}
    QSize minimumSizeHint()const override{return measuredSize();}
protected:
    void changeEvent(QEvent* event)override {
        LocalComboBox::changeEvent(event);
        if(event->type()==QEvent::FontChange||event->type()==QEvent::ApplicationFontChange||event->type()==QEvent::StyleChange){
            // Keep the existing full-label behavior for enlarged fonts.
            setMaximumWidth(qMax(260,measuredSize().width()));
            updateGeometry();
        }
    }
private:
    QSize measuredSize()const {
        int label=0;
        for(int i=0;i<count();++i)label=qMax(label,fontMetrics().horizontalAdvance(itemText(i)));
        QStyleOptionComboBox option;
        initStyleOption(&option);
        option.rect=QRect(0,0,260,CompactButtonHeight);
        const auto editor=style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,this);
        const int chrome=option.rect.width()-editor.width();
        return {qMax(200,label+chrome+1),CompactButtonHeight};
    }
};
}
void MarketCandlesView::showIndicatorPicker(){
    if(!indicatorPreferences_)return;
    ensureStudyPickerAccessibility();
    QDialog dialog(this);dialog.setProperty(StudyPickerAccessibleProperty,true);dialog.setObjectName("chartStudyPickerDialog");dialog.setWindowTitle(tr("Indicators"));
    dialog.resize(900,650);dialog.setMinimumSize(640,520);
    auto* layout=new QVBoxLayout(&dialog);layout->setSpacing(8);
    auto* filters=new QHBoxLayout;
    auto* search=new StudySearchEdit(&dialog);search->setObjectName("chartStudySearch");
    applyCompactInput(search,CompactButtonHeight);
    search->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    search->setPlaceholderText(tr("Search indicators, concepts or Korean initials"));search->setAccessibleName(tr("Search indicators and concepts"));filters->addWidget(search,1);
    auto* scope=new QTabBar(&dialog);scope->setObjectName("chartStudyScope");scope->setAccessibleName(tr("Indicator list"));
    scope->addTab(tr("All"));scope->addTab(tr("Enabled"));scope->addTab(tr("Favorites"));scope->setExpanding(false);
    applySegmentedControlStyle(scope,CompactButtonHeight);scope->setFixedHeight(CompactButtonHeight);filters->addWidget(scope);
    auto* category=new StudyCategoryCombo(&dialog);category->setObjectName("chartStudyCategory");category->setAccessibleName(tr("Category"));
    for(const auto& option:QList<QPair<QString,QString>>{{"all",tr("All Categories")},{"keefi",tr("%1 Studies").arg(productName())},{"trend",tr("Price & Trend")},
        {"momentum",tr("Momentum")},{"volume",tr("Volume & Trades")},{"futures",tr("Futures")},{"history",tr("Historical Comparison")}})category->addItem(option.second,option.first);
    filters->addWidget(category);layout->addLayout(filters);
    auto* tree=new StudyPickerTree(&dialog);tree->setObjectName("chartStudyPickerTree");tree->setColumnCount(7);
    tree->setHeaderLabels({tr("Indicator"),{},tr("Apply"),tr("Chart"),QStringLiteral("☆"),tr("Data"),tr("Settings")});tree->setRootIsDecorated(false);tree->setMinimumWidth(0);
    tree->headerItem()->setData(4,Qt::AccessibleTextRole,tr("Favorite"));tree->headerItem()->setToolTip(4,tr("Favorite"));
    tree->setAccessibleName(tr("Indicators; Space or Enter toggles Apply"));tree->setMouseTracking(true);tree->setAllColumnsShowFocus(true);
    tree->setItemDelegate(new StudySearchReasonDelegate(tree));tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    for(int column=1;column<7;++column)tree->header()->setSectionResizeMode(column,QHeaderView::ResizeToContents);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);layout->addWidget(tree,1);search->studies=tree;
    auto* empty=new QLabel(tr("No indicators match these filters."),&dialog);empty->setObjectName("chartStudyEmpty");empty->setWordWrap(true);layout->addWidget(empty);
    auto* keefi=new QTreeWidgetItem(tree,{productName()});auto* basic=new QTreeWidgetItem(tree,{tr("Basic")});
    keefi->setExpanded(true);basic->setExpanded(true);keefi->setFlags(Qt::ItemIsEnabled);basic->setFlags(Qt::ItemIsEnabled);
    const auto market=identity_.startsWith("stock:")?QStringLiteral("stock"):QStringLiteral("coin");
    QMap<QString,StudyActionButton*> helpButtons,visibilityButtons,favoriteButtons,settingsButtons;
    for(const auto& id:indicatorPreferences_->supportedIds(market)){
        auto* item=new QTreeWidgetItem(id=="volume"?basic:keefi,{studyName(id)});item->setData(0,Qt::UserRole,id);
        item->setSizeHint(0,QSize(0,40));item->setFlags(item->flags()|Qt::ItemIsUserCheckable);
        item->setCheckState(0,indicatorPreferences_->enabled(id)?Qt::Checked:Qt::Unchecked);
        auto* help=new StudyActionButton(tree);help->setProperty(StudyPickerActionProperty,true);help->setObjectName("chartStudyInfo_"+id);applyCompactButton(help,true);
        help->setIconSize(QSize(18,18));bindThemePresentation(help,[help]{help->setIcon(informationGlyph(themeColors().text));});
        help->setAccessibleName(studyName(id)+tr(" Information"));
        if(const auto definition=chartStudyHelp(id)){help->setToolTip(definition->brief);help->setAccessibleDescription(definition->brief);}
        helpButtons.insert(id,help);tree->setItemWidget(item,1,help);
        connect(help,&QPushButton::clicked,this,[this,id,help]{showIndicatorHelp(id,help);});
        auto* visible=new StudyActionButton(tree);visible->setProperty(StudyPickerActionProperty,true);visible->setObjectName("chartStudyVisible_"+id);visible->setCheckable(true);applyCompactButton(visible);visible->setIconSize(QSize(16,16));
        setThemeStyle(visible,visible->property("keefiThemeTemplate").toString()+QStringLiteral(R"(
            QPushButton[studyVisibilityAppearance="unapplied"] { background: transparent; color: ${text3}; font-weight: 400; }
            QPushButton[studyVisibilityAppearance="waiting"]:checked { background: ${bg}; color: ${text}; font-weight: 400; }
            QPushButton[studyVisibilityAppearance="waiting"]:checked:hover { background: ${surface}; }
            QPushButton[studyVisibilityAppearance="waiting"]:checked:pressed { background: ${rule}; }
        )"));
        visible->setToolTip(tr("차트에 그리는 표시만 바꿔요. 적용 여부와 자료 준비 상태는 별개예요."));
        visibilityButtons.insert(id,visible);tree->setItemWidget(item,3,visible);
        connect(visible,&QPushButton::toggled,&dialog,[this,id](bool shown){if(indicatorPreferences_)indicatorPreferences_->setVisible(id,shown);});
        auto* favorite=new StudyActionButton(tree);favorite->setProperty(StudyPickerActionProperty,true);favorite->setObjectName("chartStudyFavorite_"+id);favorite->setCheckable(true);applyCompactButton(favorite,true);
        favorite->setToolTip(tr("즐겨찾기 목록에 넣거나 빼요. 지표 적용 여부는 바뀌지 않아요."));
        favoriteButtons.insert(id,favorite);tree->setItemWidget(item,4,favorite);
        if(!ChartStudyPreferences::colorKeys(id).isEmpty()){
            auto* settings=new StudyActionButton(tree);settings->setProperty(StudyPickerActionProperty,true);
            settings->setObjectName(id=="ma"?"chartStudyMaSettings":"chartStudySettings_"+id);
            settings->setText(QStringLiteral("⚙"));settings->setToolTip(studyName(id)+" · "+tr("Settings"));
            settings->setAccessibleName(settings->toolTip());applyCompactButton(settings,true);
            settingsButtons.insert(id,settings);tree->setItemWidget(item,6,settings);
            if(id!="ma")connect(settings,&QPushButton::clicked,&dialog,[this,&dialog,id]{
                if(indicatorPreferences_)editStudyOptions(*indicatorPreferences_,id,studyName(id),&dialog);
            });
        }
        connect(favorite,&QPushButton::toggled,&dialog,[this,id](bool value){if(indicatorPreferences_)indicatorPreferences_->setFavorite(id,value);});
    }
    auto* maSettings=settingsButtons.value("ma");
    connect(maSettings,&QPushButton::clicked,&dialog,[this,&dialog]{
        if(!indicatorPreferences_)return;
        QDialog settings(&dialog);settings.setObjectName("chartStudyMaSettingsDialog");settings.setWindowTitle(tr("Moving Averages"));
        auto* column=new QVBoxLayout(&settings);auto* grid=new QGridLayout;column->addLayout(grid);
        grid->addWidget(new QLabel(tr("Line"),&settings),0,0);grid->addWidget(new QLabel(tr("Type"),&settings),0,1);grid->addWidget(new QLabel(tr("Length"),&settings),0,2);grid->addWidget(new QLabel(tr("Color"),&settings),0,3);
        QList<QCheckBox*> checks;QList<QComboBox*> kinds;QList<StudyLengthSpinBox*> periods;QList<QLabel*> lengthErrors;QList<StudyColorButton*> colors;
        const auto saved=indicatorPreferences_->movingAverages(),defaults=defaultChartMovingAverages();
        for(int i=0;i<ChartMovingAverageMaximumLines;++i){
            const auto spec=i<saved.size()?saved[i]:defaults[i];
            auto* check=new FullTargetCheckBox(&settings);check->setObjectName(QString("chartMaEnabled_%1").arg(i));applyCompactButton(check);check->setChecked(i<saved.size()&&spec.enabled);
            const auto updateLabel=[check,i](bool on){check->setText(QString::number(i+1)+"  "+(on?QStringLiteral("✓ On"):QStringLiteral("− Off")));check->setAccessibleName(QObject::tr("Moving Average %1 %2").arg(i+1).arg(on?QObject::tr("On"):QObject::tr("Off")));};
            updateLabel(check->isChecked());connect(check,&QCheckBox::toggled,&settings,updateLabel);checks.append(check);grid->addWidget(check,i+1,0);
            auto* kind=new LocalComboBox(&settings);kind->setObjectName(QString("chartMaKind_%1").arg(i));kind->setCompact();kind->addItems({"SMA","EMA","WMA"});kind->setMinimumWidth(100);kind->setCurrentIndex(int(spec.kind));kind->setAccessibleName(tr("Moving Average %1 Type").arg(i+1));kind->setAccessibleDescription(tr("Moving Average %1 Type; SMA, EMA or WMA").arg(i+1));kinds.append(kind);grid->addWidget(kind,i+1,1);
            auto* period=new StudyLengthSpinBox(&settings);period->setButtonSymbols(QAbstractSpinBox::NoButtons);period->setObjectName(QString("chartMaPeriod_%1").arg(i));period->setRange(2,ChartMovingAverageMaximumPeriod);period->setValue(spec.period);applyCompactInput(period);period->setAccessibleName(tr("Moving Average %1 Length; 2 to %2 completed candles").arg(i+1).arg(ChartMovingAverageMaximumPeriod));periods.append(period);grid->addWidget(period,i+1,2);
            setThemeStyle(period->editor(),QStringLiteral(R"(
                QLineEdit { background: ${input}; color: ${text}; border: 0; border-radius: 0; padding: 0; min-height: 0;
                    selection-background-color: ${surface}; selection-color: ${text}; }
            )"));
            bindThemePresentation(period,[period]{const auto palette=KeefiTheme::palette();period->setPalette(palette);period->editor()->setPalette(palette);});
            auto* error=new QLabel(tr("2~200 사이 숫자를 넣어 주세요"),&settings);
            error->setObjectName(QString("chartMaLengthError_%1").arg(i));
            error->setFont(KeefiFonts::font(FontRole::Caption12));
            setThemeStyle(error,"color:${danger};");
            auto policy=error->sizePolicy();policy.setRetainSizeWhenHidden(true);error->setSizePolicy(policy);
            grid->addWidget(error,i+1,4);lengthErrors.append(error);
            const auto key=QString("ma%1").arg(i+1);
            auto* color=new StudyColorButton(key,tr("Moving Average %1 Color").arg(i+1),indicatorPreferences_->studySettings().colors.value(key),&settings);
            colors.append(color);grid->addWidget(color,i+1,3);
        }
        auto* reset=new StudyActionButton(&settings);reset->setObjectName("chartMaReset");reset->setText(tr("Reset to Defaults"));applyCompactButton(reset);column->addWidget(reset);
        connect(reset,&QPushButton::clicked,&settings,[checks,kinds,periods,colors]{for(auto* color:colors)color->setColor({});const auto defaults=defaultChartMovingAverages();for(int i=0;i<defaults.size();++i){checks[i]->setChecked(defaults[i].enabled);kinds[i]->setCurrentIndex(int(defaults[i].kind));periods[i]->setValue(defaults[i].period);if(periods[i]->editor()->text().isEmpty())periods[i]->editor()->setText(QString::number(defaults[i].period));}});
        QDialogButtonBox buttons(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&settings);auto* save=buttons.button(QDialogButtonBox::Save);save->setObjectName("chartMaApply");save->setText(tr("Save"));save->setProperty("primary",true);applyCompactButton(save);buttons.button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));applyCompactButton(buttons.button(QDialogButtonBox::Cancel));column->addWidget(&buttons);
        const auto validate=[periods,lengthErrors,save]{
            bool valid=true;
            for(int i=0;i<periods.size();++i){
                auto* period=periods[i];
                const bool empty=period->editor()->text().isEmpty();
                if(period->property("error").toBool()!=empty){
                    period->setProperty("error",empty);
                    period->style()->unpolish(period);period->style()->polish(period);period->update();
                }
                lengthErrors[i]->setVisible(empty);
                valid&=!empty;
            }
            save->setEnabled(valid);
        };
        for(auto* period:periods)connect(period->editor(),&QLineEdit::textChanged,&settings,validate);
        validate();
        connect(&buttons,&QDialogButtonBox::accepted,&settings,[this,&settings,checks,kinds,periods,colors,save]{
            if(!save->isEnabled()||!indicatorPreferences_)return;
            QList<ExtendedMaSpec> values;
            auto options=indicatorPreferences_->studySettings();
            for(int i=0;i<checks.size();++i){
                const auto key=QString("ma%1").arg(i+1);
                values.append({key,periods[i]->value(),ExtendedMaKind(kinds[i]->currentIndex()),checks[i]->isChecked()});
                if(colors[i]->color().isValid())options.colors.insert(key,colors[i]->color());else options.colors.remove(key);
            }
            if(indicatorPreferences_->setStudySettings(options,values))settings.accept();
        });
        connect(&buttons,&QDialogButtonBox::rejected,&settings,&QDialog::reject);
        for(int i=0;i<checks.size();++i){QWidget::setTabOrder(checks[i],kinds[i]);QWidget::setTabOrder(kinds[i],periods[i]);QWidget::setTabOrder(periods[i],colors[i]);QWidget::setTabOrder(colors[i],i+1<checks.size()?static_cast<QWidget*>(checks[i+1]):static_cast<QWidget*>(reset));}QWidget::setTabOrder(reset,save);
        settings.resize(580,380);settings.exec();
    });
    const auto filter=[this,tree,search,scope,category,empty,keefi,basic](bool reselect){
        const auto query=search->text().trimmed(),queryKey=chartStudySearchKey(query),selectedCategory=category->currentData().toString();
        QTreeWidgetItem* first=nullptr;QTreeWidgetItem* best=nullptr;int bestRank=-1;bool bestExact=false,currentVisible=false,visibilityChanged=false;
        const auto setHidden=[&visibilityChanged](QTreeWidgetItem* item,bool hidden){if(item->isHidden()!=hidden){item->setHidden(hidden);visibilityChanged=true;}};
        {const QSignalBlocker block(tree);
            for(auto* group:{keefi,basic}){bool any=false;for(int i=0;i<group->childCount();++i){auto* item=group->child(i);const auto id=item->data(0,Qt::UserRole).toString();
                const auto result=matchChartStudy(id,query);
                const bool match=result.matched&&(selectedCategory=="all"||studyCategory(id)==selectedCategory)&&
                    (scope->currentIndex()==0||(indicatorPreferences_&&(scope->currentIndex()==1?indicatorPreferences_->enabled(id):indicatorPreferences_->favorite(id))));
                item->setData(0,Qt::UserRole+1,result.reason);
                const bool on=indicatorPreferences_&&indicatorPreferences_->enabled(id),shown=indicatorPreferences_&&indicatorPreferences_->visible(id);
                auto accessible=studyName(id)+", "+(on?tr("On"):tr("Off"))+", "+(shown?tr("Visible"):tr("Hidden"))+", "+item->text(5);
                if(!result.reason.isEmpty())accessible+=tr(", 찾은 이유 %1").arg(result.reason);
                item->setData(0,Qt::AccessibleTextRole,accessible);
                item->setToolTip(0,result.reason.isEmpty()?QString{}:tr("찾은 이유 · %1").arg(result.reason));
                setHidden(item,!match);any|=match;
                if(match){if(!first)first=item;const bool exact=!queryKey.isEmpty()&&chartStudySearchKey(item->text(0))==queryKey;
                    if(result.rank>bestRank||(result.rank==bestRank&&exact&&!bestExact)){best=item;bestRank=result.rank;bestExact=exact;}
                    if(tree->currentItem()==item)currentVisible=true;}
            }setHidden(group,!any);}
        }
        // Flush only real filtering changes before native AX inspects cell state.
        if(visibilityChanged)tree->doItemsLayout();
        empty->setVisible(!first);
        if((reselect&&!query.isEmpty())||!currentVisible)tree->setCurrentItem(query.isEmpty()?first:best);
    };
    const auto refresh=[this,tree,search,visibilityButtons,favoriteButtons,filter]{
        auto searchPalette=search->palette();searchPalette.setColor(QPalette::Text,themeColors().text);searchPalette.setColor(QPalette::PlaceholderText,themeColors().text3);
        if(search->palette()!=searchPalette)search->setPalette(searchPalette);
        QMap<QString,ChartIndicatorState> states;for(const auto& state:indicatorStates())states.insert(state.id,state);
        {const QSignalBlocker block(tree);for(QTreeWidgetItemIterator it(tree);*it;++it){auto* item=*it;const auto id=item->data(0,Qt::UserRole).toString();if(id.isEmpty())continue;
            const bool on=indicatorPreferences_&&indicatorPreferences_->enabled(id),shown=indicatorPreferences_&&indicatorPreferences_->visible(id),starred=indicatorPreferences_&&indicatorPreferences_->favorite(id);
            if(item->checkState(0)!=(on?Qt::Checked:Qt::Unchecked))item->setCheckState(0,on?Qt::Checked:Qt::Unchecked);
            const auto apply=on?QStringLiteral("✓ On"):QStringLiteral("− Off");if(item->text(2)!=apply)item->setText(2,apply);
            item->setData(2,Qt::AccessibleTextRole,studyName(id)+" "+apply);
            const auto state=states.value(id);
            auto* visible=visibilityButtons.value(id);const QSignalBlocker visibleBlock(visible);visible->setChecked(shown);visible->setText(shown?tr("Visible"):tr("Hidden"));
            const auto appearance=!on?QStringLiteral("unapplied"):state.requiredReady?QStringLiteral("ready"):QStringLiteral("waiting");
            if(visible->property("studyVisibilityAppearance").toString()!=appearance){
                visible->setProperty("studyVisibilityAppearance",appearance);
                visible->style()->unpolish(visible);visible->style()->polish(visible);visible->update();
            }
            visible->setAccessibleName(studyName(id)+(shown?tr(" Visible"):tr(" Hidden")));const auto& icon=shown?indicatorEyeShownIcon_:indicatorEyeHiddenIcon_;if(visible->icon().cacheKey()!=icon.cacheKey())visible->setIcon(icon);
            auto* favorite=favoriteButtons.value(id);const QSignalBlocker favoriteBlock(favorite);favorite->setChecked(starred);favorite->setText(starred?QStringLiteral("★"):QStringLiteral("☆"));favorite->setAccessibleName(studyName(id)+(starred?tr(" Favorite"):tr(" Add to Favorites")));
            const auto data=!on?tr("Not calculated"):
                state.dataDetail==IndicatorDataDetail::NoSignal?tr("No signal"):
                state.dataDetail==IndicatorDataDetail::Partial?tr("Partial"):
                state.requiredReady?tr("Ready"):tr("Needs data");
            if(item->text(5)!=data)item->setText(5,data);
            item->setFont(0,KeefiFonts::font(on?FontRole::BodyStrong15:FontRole::Body15));
            item->setForeground(0,!on?themeColors().text3:state.requiredReady&&!shown?themeColors().text2:themeColors().text);
            item->setFont(2,KeefiFonts::font(on?FontRole::SecondaryStrong14:FontRole::Secondary14));
            item->setFont(5,KeefiFonts::font(FontRole::Caption12));
            item->setForeground(5,on&&!state.requiredReady?themeColors().accentText:themeColors().text2);
            item->setToolTip(5,state.reason.isEmpty()?state.status:state.status+"\n"+state.reason);
            item->setData(5,Qt::AccessibleTextRole,studyName(id)+", "+data);
        }}filter(false);
    };
    connect(tree,&QTreeWidget::itemChanged,&dialog,[this,refresh](QTreeWidgetItem* item,int column){if(column!=0||!indicatorPreferences_)return;const auto id=item->data(0,Qt::UserRole).toString();if(!id.isEmpty()){indicatorPreferences_->setEnabled(id,item->checkState(0)==Qt::Checked);refresh();}});
    connect(search,&QLineEdit::textChanged,&dialog,[filter]{filter(true);});connect(scope,&QTabBar::currentChanged,&dialog,[filter]{filter(true);});connect(category,&QComboBox::currentIndexChanged,&dialog,[filter]{filter(true);});
    connect(indicatorPreferences_,&ChartStudyPreferences::changed,&dialog,refresh);
    connect(this,&MarketCandlesView::studiesChanged,&dialog,refresh);connect(this,&MarketCandlesView::coinFlowStudiesChanged,&dialog,refresh);
    bindThemePresentation(&dialog,refresh);
    auto* focusSearch=new QShortcut(QKeySequence(QStringLiteral("/")),&dialog);connect(focusSearch,&QShortcut::activated,search,[search]{search->setFocus();search->selectAll();});
    auto* hint=new QLabel(tr("↑↓ Navigate · Space/Enter Apply · Tab Actions · / Search · Esc Close"),&dialog);hint->setWordWrap(true);layout->addWidget(hint);
    QDialogButtonBox buttons(QDialogButtonBox::Close,&dialog);buttons.button(QDialogButtonBox::Close)->setText(tr("Close"));applyCompactButton(buttons.button(QDialogButtonBox::Close));buttons.button(QDialogButtonBox::Close)->setAutoDefault(false);layout->addWidget(&buttons);connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QWidget::setTabOrder(search,scope);QWidget::setTabOrder(scope,category);QWidget::setTabOrder(category,tree);
    QWidget* previous=tree;
    for(const auto& id:indicatorPreferences_->supportedIds(market)){QWidget::setTabOrder(previous,helpButtons.value(id));QWidget::setTabOrder(helpButtons.value(id),visibilityButtons.value(id));QWidget::setTabOrder(visibilityButtons.value(id),favoriteButtons.value(id));previous=favoriteButtons.value(id);if(auto* settings=settingsButtons.value(id)){QWidget::setTabOrder(previous,settings);previous=settings;}}
    QWidget::setTabOrder(previous,buttons.button(QDialogButtonBox::Close));
    refresh();search->setFocus();dialog.exec();QTimer::singleShot(0,this,[this]{window()->activateWindow();indicatorPicker_->setFocus(Qt::OtherFocusReason);});
}
void MarketCandlesView::showIndicatorHelp(const QString& id,QWidget* returnFocus){
    if(studyName(id).isEmpty())return;
    if(indicatorHelpDialog_){indicatorHelpDialog_->raise();return;}
    QPointer<QWidget> back=returnFocus?returnFocus:QApplication::focusWidget();if(!back)back=window()->focusWidget();
    auto* owner=QApplication::activeModalWidget();QDialog dialog(owner?owner:this);indicatorHelpDialog_=&dialog;
    dialog.setObjectName("chartStudyHelpDialog");dialog.setWindowTitle(studyName(id)+tr(" · Information"));dialog.setFont(KeefiFonts::font(FontRole::Body15));
    auto* layout=new QVBoxLayout(&dialog);
    auto* heading=new QLabel(dialog.windowTitle(),&dialog);heading->setFont(KeefiFonts::font(FontRole::Title17));heading->setTextFormat(Qt::PlainText);layout->addWidget(heading);
    auto* scroll=new QScrollArea(&dialog);scroll->setObjectName("chartStudyHelpScroll");scroll->setWidgetResizable(true);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);scroll->setFrameShape(QFrame::NoFrame);
    auto* content=new QWidget(scroll);auto* rows=new QVBoxLayout(content);rows->setContentsMargins(8,8,8,8);rows->setSpacing(12);
    const auto section=[&](const QString& heading,const QString& value,const char* name){
        if(!heading.isEmpty()){auto* title=new LedgerTextLabel(FontRole::BodyStrong15,content);title->setText(heading);rows->addWidget(title);}
        auto* text=new LedgerTextLabel(FontRole::Body15,content);text->setObjectName(name);text->setWordWrap(true);text->setTextFormat(Qt::PlainText);text->setMinimumWidth(0);text->setText(value);rows->addWidget(text);return text;
    };
    const auto definition=chartStudyHelp(id);
    if(definition){
        section({},definition->brief,"chartStudyHelp_brief");section(tr("쓰는 자료"),definition->inputs,"chartStudyHelp_inputs");
        section(tr("상태 읽기"),definition->reading,"chartStudyHelp_reading");section(tr("한계"),definition->limitations,"chartStudyHelp_limits");
    }
    ChartIndicatorState selected;for(const auto& state:indicatorStates())if(state.id==id){selected=state;break;}
    section(tr("열었을 때 상태"),selected.enabled?selected.status:tr("꺼짐 · 설명을 읽어도 켜지지 않아요."),"chartStudyHelp_state");
    QStringList details;
    if(definition)details<<definition->calculation;
    if(id=="ma"&&indicatorPreferences_){QStringList configured;const auto values=indicatorPreferences_->movingAverages();for(int i=0;i<values.size();++i){const auto& value=values[i];configured<<tr("선 %1 · %2 %3 · %4").arg(i+1).arg(value.kind==ExtendedMaKind::Simple?QStringLiteral("SMA"):value.kind==ExtendedMaKind::Exponential?QStringLiteral("EMA"):QStringLiteral("WMA")).arg(value.period).arg(value.enabled?tr("사용"):tr("사용 안 함"));}details<<tr("현재 저장된 설정")+"\n"+configured.join('\n');}
    if(indicatorPreferences_){
        const auto& options=indicatorPreferences_->studySettings();
        if(id=="bb")details<<tr("차트 표시 설정 · Length %1 · 표준편차 배수 %2").arg(options.displayBandPeriod).arg(options.displayBandDeviations);
        if(id=="rsi")details<<tr("현재 저장된 설정 · RSI %1").arg(options.rsiPeriod);
        if(id=="macd")details<<tr("현재 저장된 설정 · MACD %1 / %2 / %3").arg(options.macdFast).arg(options.macdSlow).arg(options.macdSignal);
    }
    if(!selected.reason.isEmpty())details<<selected.reason;
    for(const auto& atom:selected.atoms)if(!atom.reason.isEmpty()&&!details.contains(atom.reason))details<<atom.reason;
    if(id=="whales"){
        // This detail is built solely from the selected public trade DTO; do
        // not attribute its actual 1m facts to the chart's chosen timeframe.
        QTextDocument document;document.setHtml(coinFlowDetails_->text());if(!document.toPlainText().isEmpty())details<<document.toPlainText();
    }else{
        const auto data=identity_.startsWith("stock:")?stockSnapshot_.series:model_?model_->series():MarketCandleSeries{};
        if(data.identity==identity_&&data.intervalMs==intervalMs_&&data.available){
            QStringList source{tr("선택 차트 · %1 · 완료 봉 %2개").arg(timeframeLabel(intervalMs_)).arg(data.candles.size())};
            if((id=="ma"||id=="bb")&&displayExtendedSnapshot_.observations>data.candles.size())
                source<<tr("선·밴드 계산용 완료 봉 %1개 · 알림·성과의 관측 범위는 그대로예요.").arg(displayExtendedSnapshot_.observations);
            if(!data.sourceUrl.isEmpty())source<<data.sourceUrl;
            if(data.sourceTimeMs>0)source<<tr("원천 시각 %1").arg(sourceTime(data.sourceTimeMs));
            if(data.receivedAtMs>0)source<<tr("수신 시각 %1").arg(sourceTime(data.receivedAtMs));
            details<<source.join('\n');
        }
        if(id=="similarity"){
            const auto& comparison=studySnapshot_.similarity;
            details<<tr("검색 %1봉 · 채택 %2개 · 겹침 제외 %3개").arg(comparison.searchBars).arg(comparison.samples).arg(comparison.overlapDropped);
            for(int i=0;i<comparison.paths.size();++i){const auto& path=comparison.paths[i];details<<tr("경로 %1 · 상관 %2\n비교 %3 ~ %4\n후속 완료 %5").arg(i+1).arg(QString::number(path.correlation,'f',3),sourceTime(path.fromMs),sourceTime(path.toMs),sourceTime(path.followingToMs));}
        }
        if(!definition){const auto existing=findChild<QLabel*>("marketCandlesStudyDetails")->text();if(!existing.isEmpty())details<<tr("공통 차트 계산 상세 · 다른 지표의 설명도 포함돼요.")+"\n"+existing;}
    }
    const auto detailsLabel=tr("Sources & Calculation");
    const auto detailsButtonText=QString(detailsLabel).replace(QLatin1Char('&'),QStringLiteral("&&"));
    auto* expand=new QPushButton(QStringLiteral("▸ ")+detailsButtonText,content);expand->setObjectName("chartStudyHelpDetails");
    expand->setAccessibleName(detailsLabel);expand->setCheckable(true);applyCompactButton(expand);rows->addWidget(expand);
    connect(expand,&QPushButton::toggled,expand,[expand,detailsButtonText](bool open){expand->setText((open?QStringLiteral("▾ "):QStringLiteral("▸ "))+detailsButtonText);});
    auto* calculation=section({},details.isEmpty()?tr("확인된 계산 자료가 없어요."):details.join("\n\n"),"chartStudyHelp_calculation");calculation->hide();
    connect(expand,&QPushButton::toggled,calculation,&QWidget::setVisible);rows->addStretch();scroll->setWidget(content);layout->addWidget(scroll,1);
    QDialogButtonBox buttons(QDialogButtonBox::Close,&dialog);auto* close=buttons.button(QDialogButtonBox::Close);close->setText(tr("Close"));applyCompactButton(close);layout->addWidget(&buttons);connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    const auto room=screen()?screen()->availableGeometry().size():QSize(1024,800);dialog.setMaximumHeight(480);dialog.resize(qMin(520,room.width()-40),qMin(480,room.height()-80));close->setFocus();dialog.exec();indicatorHelpDialog_=nullptr;
    QTimer::singleShot(0,this,[back]{if(back&&back->isVisible()&&back->isEnabled()){back->window()->activateWindow();back->setFocus(Qt::OtherFocusReason);}});
}
void MarketCandlesView::maybeShowIndicatorIntro(){
    if(!indicatorPreferences_||!isVisible()||indicatorPreferences_->introSeen()||introDisplayed_)return;
    introDisplayed_=true;introDismissed_=false;indicatorIntro_->show();indicatorPreferences_->markIntroSeen();
}

} // namespace keefi
