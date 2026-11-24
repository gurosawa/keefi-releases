#include "keefi/ChartAlertsPanel.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/LedgerTextHighlighter.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/MovementColorPolicy.h"
#include <QCheckBox>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QDir>
#include <QDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPaintEngine>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimeEdit>
#include <QVBoxLayout>
using namespace keefi;
namespace {
class Sink final:public SystemNotificationSink {
  public:
    int requests=0,posts=0;
    SystemNoticePermission permission()const override{return SystemNoticePermission::Unknown;}
    QString status()const override{return "합성 권한 미확인";}
    void refreshPermission()override{}
    void requestAuthorization()override{++requests;}
    void post(const SystemNotice&)override{++posts;}
    void remove(const QStringList&)override{}
    void setPrivateContext(quint64,bool)override{}
    void stop()override{}
};
bool saveD5(QWidget& widget,const QString& name){
    const auto path=qEnvironmentVariable("KEEFI_D5_RENDER_DIR");if(path.isEmpty())return true;
    if(!QDir().mkpath(path))return false;QCoreApplication::processEvents();
    return widget.grab().save(QDir(path).filePath(name+".png"));
}
QByteArray syntheticReviewPack(){
    auto object=QJsonDocument::fromJson(ChartPhrasePack::plainJson()).object();
    object["id"]="synthetic_panel_review";object["name"]=QStringLiteral("합성 패널 검수 팩");
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
// Record fonts from actual widget and popup painting. A combo's base font
// cannot prove that its Korean and numeric text runs have different roles.
class AlertControlPaintEngine final:public QPaintEngine {
  public:
    struct Run{QString text;QFont font;};QList<Run> runs;
    bool begin(QPaintDevice*)override{return true;}bool end()override{return true;}
    Type type()const override{return User;}
    void updateState(const QPaintEngineState&)override{}
    void drawPixmap(const QRectF&,const QPixmap&,const QRectF&)override{}
    void drawPath(const QPainterPath&)override{}
    void drawPolygon(const QPointF*,int,PolygonDrawMode)override{}
    void drawTextItem(const QPointF&,const QTextItem& item)override{runs.append({item.text(),item.font()});}
};
class AlertControlPaintDevice final:public QPaintDevice {
  public:
    mutable AlertControlPaintEngine engine;
    QPaintEngine* paintEngine()const override{return &engine;}
    int metric(PaintDeviceMetric value)const override{
        switch(value){case PdmWidth:return 1600;case PdmHeight:return 1600;case PdmDepth:return 32;case PdmNumColors:return 0;
        case PdmDevicePixelRatio:return 1;case PdmDevicePixelRatioScaled:return int(devicePixelRatioFScale());default:return 96;}
    }
};
QList<AlertControlPaintEngine::Run> paintedControl(QWidget& widget){AlertControlPaintDevice device;widget.render(&device);return device.engine.runs;}
bool paintsControlRole(const QList<AlertControlPaintEngine::Run>& runs,const QString& pattern,FontRole role){
    const auto expected=KeefiFonts::font(role);const QRegularExpression match(pattern);
    for(const auto& run:runs)if(match.match(run.text).hasMatch()&&run.font.family()==expected.family()&&
        run.font.pixelSize()==expected.pixelSize()&&run.font.weight()==expected.weight())return true;
    return false;
}
}
class ChartAlertsPanelTest final:public QObject {
    Q_OBJECT
  private slots:
    void alertActionsHaveFortyFourPixelTargets_data();
    void alertActionsHaveFortyFourPixelTargets();
    void nonEditableAlertControlsPaintTheirMixedRoles_data();
    void nonEditableAlertControlsPaintTheirMixedRoles();
    void alertHistoryActuallyPaintsEachOriginalLine_data();
    void alertHistoryActuallyPaintsEachOriginalLine();
    void typographyUsesBundledRolesWithoutChangingAlertState_data();
    void typographyUsesBundledRolesWithoutChangingAlertState();
    void mixedLabelsPreserveLongPlainTextSizingAndSelection();
    void cleanup(){KeefiTheme::instance()->setMode(ThemeMode::System);}
    void dripDefaultCanBeDisabledImmediatelyWithoutSystemEffects();
    void permissionRequiresSettingsActionAndIntroIsRemembered();
    void privateHistoryIsErasedAndHiddenUpdatesCatchUpOnShow();
    void uiPreferencesContainNoHistoryAndRejectUnknownFields();
    void phrasePackRequiresHumanReviewAndInvalidPackDoesNotApply();
    void reviewedPublicPackShaRoundTripsThroughTemporaryProfile();
    void listRulesQuietHoursAndDailyCapEditTheInjectedEngine();
    void unavailableBackgroundDisablesControlAndExplainsCloseQuits();
    void whaleAlertIsExplicitOptInAndKeepsTheStudyTimeframeIndependent();
    void horizontalIsPublicListOptInWithoutEnablingOtherKinds();
    void horizontalListSwitchDoesNotWriteAndHoldingsCannotEnableIt();
    void acceptedTypedMovementRecolorsExistingHistoryAndKeepsRiskFixed_data();
    void acceptedTypedMovementRecolorsExistingHistoryAndKeepsRiskFixed();
    void hiddenTypedHistoryCatchesUpAndPrivateMetadataNeverPersists();
    void embeddedPageSurvivesOptionRecreationAndRetargetsRules();
    void hiddenControllerUsesStaticBoundedHistoryAndErasesAccessiblePrivateText();
    void unshownPageDoesNotConsumeIntroOrRequestPermission();
    void settingsEntrypointRoutesAndEmbeddedReviewFollowsOptions();
    void deletingControllerRevokesDisposablePageCallbacks();
};
void ChartAlertsPanelTest::alertActionsHaveFortyFourPixelTargets_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void ChartAlertsPanelTest::alertActionsHaveFortyFourPixelTargets(){
    QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);
    // Include the same inherited global padding and native styles as the app.
    panel.setStyleSheet(KeefiTheme::localStyleSheet(&panel));panel.resize(980,148);panel.show();QCoreApplication::processEvents();
    auto* drip=panel.findChild<QCheckBox*>("chartDripEnabled");auto* settings=panel.findChild<QPushButton*>("chartAlertSettings");QVERIFY(drip);QVERIFY(settings);
    for(auto* control:QList<QWidget*>{drip,settings})QVERIFY2(control->height()>=44,qPrintable(control->objectName()+" height="+QString::number(control->height())));
    // The added target must be clickable, not just transparent layout space.
    QVERIFY(drip->isChecked());QTest::mouseClick(drip,Qt::LeftButton,Qt::NoModifier,QPoint(drip->width()-2,2));QVERIFY(!engine.settings().dripEnabled);
    QTest::mouseClick(settings,Qt::LeftButton,Qt::NoModifier,QPoint(settings->width()/2,2));QCoreApplication::processEvents();
    auto* dialog=panel.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);QVERIFY(dialog->isVisible());
    auto* scroll=dialog->findChild<QScrollArea*>();QVERIFY(scroll);QVERIFY(scroll->verticalScrollBar()->maximum()>0);
    for(auto* control:dialog->findChildren<QWidget*>()){
        if(!qobject_cast<QPushButton*>(control)&&!qobject_cast<QCheckBox*>(control)&&!qobject_cast<QComboBox*>(control)&&!qobject_cast<QAbstractSpinBox*>(control))continue;
        if(!control->isVisible())continue;
        QVERIFY2(control->height()>=44,qPrintable(QString(control->metaObject()->className())+" "+control->objectName()+" height="+QString::number(control->height())));
    }
    auto* strong=dialog->findChild<QCheckBox*>("chartObservationAlert_strong_reversal");QVERIFY(strong);scroll->ensureWidgetVisible(strong);QCoreApplication::processEvents();
    QVERIFY(!engine.observationEnabled("strong_reversal"));QTest::mouseClick(strong,Qt::LeftButton,Qt::NoModifier,QPoint(strong->width()-2,2));QVERIFY(engine.observationEnabled("strong_reversal"));
    QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::nonEditableAlertControlsPaintTheirMixedRoles_data(){
    QTest::addColumn<bool>("dark");QTest::addColumn<QString>("surface");
    for(const bool dark:{false,true})for(const auto* surface:{"collapsed","popup","mute-button"})
        QTest::newRow(qPrintable(QString(dark?"dark-":"light-")+surface))<<dark<<QString(surface);
}
void ChartAlertsPanelTest::nonEditableAlertControlsPaintTheirMixedRoles(){
    QFETCH(bool,dark);QFETCH(QString,surface);KeefiTheme::instance()->setSettings(nullptr);
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    constexpr qint64 now=1792713600000;Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[]{return now;});
    QVERIFY(engine.setPublicLists({{"synthetic-f10",{{"coin","upbit","KRW-SYN"}}}}));
    ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);panel.setListLabels({{"synthetic-f10",QStringLiteral("합성 공개 리스트")}});
    panel.setStyleSheet(KeefiTheme::localStyleSheet(&panel));panel.resize(980,280);panel.show();panel.openSettings("synthetic-f10");QCoreApplication::processEvents();
    auto* dialog=panel.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);
    auto* timeframe=dialog->findChild<QComboBox*>("chartAlertTimeframe");QVERIFY(timeframe);QVERIFY(!timeframe->isEditable());
    QCOMPARE(timeframe->currentText(),QStringLiteral("15분"));QCOMPARE(timeframe->currentData().toLongLong(),qint64(900000));
    auto* cooldown=dialog->findChild<QSpinBox*>("chartAlertCooldown");QVERIFY(cooldown);
    QCOMPARE(cooldown->suffix(),QStringLiteral("분"));QCOMPARE(cooldown->font(),KeefiFonts::font(FontRole::Body15));
    const auto before=engine.settings();QList<AlertControlPaintEngine::Run> runs;
    QPushButton* mute=nullptr;QWidget* painted=timeframe;
    if(surface=="mute-button"){
        for(auto* button:dialog->findChildren<QPushButton*>())if(button->text()==QStringLiteral("알림 1시간 쉬기"))mute=button;
        QVERIFY(mute);painted=mute;runs=paintedControl(*mute);QCOMPARE(mute->text(),QStringLiteral("알림 1시간 쉬기"));
        QPushButton native(mute->text(),mute->parentWidget());native.setFont(mute->font());native.ensurePolished();native.resize(mute->size());
        QCOMPARE(mute->sizeHint(),native.sizeHint());QCOMPARE(mute->minimumSizeHint(),native.minimumSizeHint());
        QString originalPainted;for(const auto& run:paintedControl(native))originalPainted+=run.text;QCOMPARE(originalPainted,mute->text());
        QString mixedPainted;for(const auto& run:runs)mixedPainted+=run.text;
        QStyleOptionButton option;option.initFrom(mute);option.text=mute->text();
        QTextLayout measured(mute->text(),mute->font());measured.setFormats(KeefiFonts::numberRuns(mute->text()));measured.beginLayout();auto measuredLine=measured.createLine();measuredLine.setLineWidth(1000000);measured.endLayout();
        qInfo()<<"F10 synthetic mute geometry"<<mute->size()<<mute->sizeHint()<<mute->style()->subElementRect(QStyle::SE_PushButtonContents,&option,mute)
            <<"native/mixed text width"<<QFontMetricsF(mute->font()).horizontalAdvance(mute->text())<<measuredLine.naturalTextWidth();
        QCOMPARE(mixedPainted,mute->text());
        const auto original=mute->text();const auto size=mute->size();const auto beforeInk=mute->grab().toImage();
        // A full drawTextItem alone could still be clipped. Change only the
        // final action character and require its actual button pixels to move.
        mute->setText(original.left(original.size()-1)+QStringLiteral("다"));const auto afterInk=mute->grab().toImage();mute->setText(original);
        QCOMPARE(mute->size(),size);QVERIFY2(beforeInk!=afterInk,"The final action character must have visible ink inside the unchanged button bounds");
    }else if(surface=="popup"){
        timeframe->showPopup();QCoreApplication::processEvents();QVERIFY(timeframe->view()->isVisible());
        QCOMPARE(QApplication::activePopupWidget(),timeframe->view()->window());painted=timeframe->view()->viewport();runs=paintedControl(*painted);
    }else runs=paintedControl(*timeframe);
    QVERIFY2(paintsControlRole(runs,QStringLiteral("[0-9]"),FontRole::Number11),qPrintable(surface+QStringLiteral(" must actually paint numeric glyphs with Departure Mono11")));
    QVERIFY2(paintsControlRole(runs,QStringLiteral("[가-힣]"),FontRole::Body15),qPrintable(surface+QStringLiteral(" must retain Pretendard15 for Korean")));
    const auto renderPath=qEnvironmentVariable("KEEFI_F10_RENDER_DIR");
    if(!renderPath.isEmpty()){QVERIFY(QDir().mkpath(renderPath));QVERIFY(painted->grab().save(QDir(renderPath).filePath(QString(dark?"dark-":"light-")+surface+".png")));}
    if(surface=="mute-button"){
        mute->click();QCOMPARE(engine.settings().mutedUntilMs,now+3600000);QCOMPARE(timeframe->currentText(),QStringLiteral("15분"));
        QCOMPARE(engine.rule("synthetic-f10").intervalMs,qint64(900000));
    }else{
        if(surface=="popup"){QTest::keyClick(timeframe->view(),Qt::Key_Down);QTest::keyClick(timeframe->view(),Qt::Key_Return);}
        else{timeframe->setFocus();QTest::keyClick(timeframe,Qt::Key_Down);}
        QCOMPARE(timeframe->currentText(),QStringLiteral("1시간"));QCOMPARE(timeframe->currentData().toLongLong(),qint64(3600000));
        QCOMPARE(engine.rule("synthetic-f10").intervalMs,qint64(3600000));QCOMPARE(engine.settings().mutedUntilMs,before.mutedUntilMs);
    }
    QCOMPARE(engine.rule("synthetic-f10").cooldownMs,qint64(15*60000));QCOMPARE(engine.settings().quietEnabled,before.quietEnabled);
    QCOMPARE(engine.settings().dailyLimit,before.dailyLimit);QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::alertHistoryActuallyPaintsEachOriginalLine_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void ChartAlertsPanelTest::alertHistoryActuallyPaintsEachOriginalLine(){
    QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);
    panel.resize(980,280);panel.show();emit engine.noticeSubmitted("synthetic-two-lines","합성 BTC/USDT","두 번째 줄 FIRST123",false);
    auto* history=panel.findChild<QListWidget*>("chartAlertHistory");QVERIFY(history);QCOMPARE(history->count(),1);QCoreApplication::processEvents();
    QVERIFY2(history->item(0)->toolTip().contains(QStringLiteral("두 번째 줄 FIRST123")),"Elided alert bodies must remain available in the row tooltip");
    const auto rect=history->visualItemRect(history->item(0));const auto before=history->viewport()->grab().toImage();
    // Only the body changes. If the second original line is not painted, the
    // public viewport image stays identical even though the model text changed.
    history->item(0)->setText(QStringLiteral("시스템 등록 · 합성 BTC/USDT\n두 번째 줄 SECOND456"));QCoreApplication::processEvents();
    const auto after=history->viewport()->grab().toImage();QCOMPARE(history->visualItemRect(history->item(0)),rect);
    QVERIFY2(before!=after,"Changing only the alert body must change the actual painted history viewport");
    QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::mixedLabelsPreserveLongPlainTextSizingAndSelection(){
    const auto text=QStringLiteral("원문 <tag>  12:05 KST · BTC/USDT\n공개 원천 시각과 84,672 관측값을 유지하는 긴 설명입니다. ").repeated(3);
    LedgerTextLabel label(text,FontRole::Body15);QLabel reference(text);reference.setFont(KeefiFonts::font(FontRole::Body15));reference.setTextFormat(Qt::PlainText);
    for(auto* item:{static_cast<QLabel*>(&label),&reference}){item->setWordWrap(true);item->resize(280,280);}
    QCOMPARE(label.text(),text);QCOMPARE(label.textFormat(),Qt::PlainText);QCOMPARE(label.sizeHint(),reference.sizeHint());
    QCOMPARE(label.heightForWidth(280),reference.heightForWidth(280));QVERIFY(label.heightForWidth(280)>label.heightForWidth(560));
    // Selectable mixed text uses QLabel's real rich text document. Its layout,
    // selection hit testing and painted font runs all belong to Qt.
    label.setTextFormat(Qt::RichText);label.setText(KeefiFonts::mixedTextHtml(text,FontRole::Body15));
    label.setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);
    QTextDocument rendered;rendered.setHtml(label.text());QCOMPARE(rendered.toPlainText(),text);
    label.setSelection(3,20);QCOMPARE(label.selectedText(),text.mid(3,20));
    label.setSelection(0,text.size());auto selected=label.selectedText();selected.replace(QChar::ParagraphSeparator,QLatin1Char('\n'));selected.replace(QChar::LineSeparator,QLatin1Char('\n'));QCOMPARE(selected,text);
    label.show();QCoreApplication::processEvents();QVERIFY(!label.grab().isNull());
    label.setSelection(0,0);QVERIFY(!label.hasSelectedText());QVERIFY(!label.grab().isNull());
    const auto size=label.sizeHint();const auto height=label.heightForWidth(280);KeefiTheme::instance()->setMode(ThemeMode::Dark);QCoreApplication::processEvents();
    rendered.setHtml(label.text());QCOMPARE(rendered.toPlainText(),text);QCOMPARE(label.sizeHint(),size);QCOMPARE(label.heightForWidth(280),height);
    QTextDocument document;document.setDefaultFont(KeefiFonts::font(FontRole::Body15));LedgerTextHighlighter highlighter(&document);
    document.setPlainText(text);highlighter.rehighlight();QCOMPARE(document.toPlainText(),text);
    const auto formats=document.firstBlock().layout()->formats();bool number=false;
    for(const auto& run:formats)if(document.firstBlock().text().mid(run.start,run.length)==QStringLiteral("12:05")){
        number=true;QCOMPARE(run.format.font().families().first(),KeefiFonts::numberFamily());QCOMPARE(run.format.font().pixelSize(),KeefiFonts::font(FontRole::Number11).pixelSize());}
    QVERIFY(number);
}
void ChartAlertsPanelTest::typographyUsesBundledRolesWithoutChangingAlertState_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void ChartAlertsPanelTest::typographyUsesBundledRolesWithoutChangingAlertState(){
    QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);
    panel.resize(980,280);panel.show();QCoreApplication::processEvents();
    const auto before=engine.publicState();
    auto* status=panel.findChild<QLabel*>("chartAlertStatus");auto* history=panel.findChild<QListWidget*>("chartAlertHistory");
    QVERIFY(status);QVERIFY(history);QCOMPARE(status->font(),KeefiFonts::font(FontRole::Caption12));
    QCOMPARE(history->font(),KeefiFonts::font(FontRole::Body15));
    QCOMPARE(panel.findChild<QCheckBox*>("chartDripEnabled")->font(),KeefiFonts::font(FontRole::Body15));
    emit engine.noticeSubmitted("synthetic-type","합성 BTC/USDT","관측 12:05 KST · 84,672 · 2.50%",false,MovementDirection::Unknown,true);
    QCOMPARE(history->count(),1);QCOMPARE(history->item(0)->text(),QStringLiteral("시스템 등록 · 합성 BTC/USDT\n관측 12:05 KST · 84,672 · 2.50%"));
    QCOMPARE(history->item(0)->foreground().color(),themeColors().danger);
    const auto theme=dark?QStringLiteral("dark"):QStringLiteral("light");
    panel.setStyleSheet(KeefiTheme::localStyleSheet(&panel));QVERIFY(saveD5(panel,"alert-feed-"+theme));
    panel.openSettings();auto* dialog=panel.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);
    QCOMPARE(dialog->font(),KeefiFonts::font(FontRole::Body15));
    QCOMPARE(dialog->findChild<QLabel*>("chartDripIntroduction")->font(),KeefiFonts::font(FontRole::Secondary14));
    for(const auto* name:{"chartAlertMinimumScore","chartAlertDailyLimit"}){
        auto* field=dialog->findChild<QSpinBox*>(name);QVERIFY(field);QCOMPARE(field->font(),KeefiFonts::font(FontRole::Number11));
    }
    QCOMPARE(dialog->findChild<QDoubleSpinBox*>("chartAlertDistance")->font(),KeefiFonts::font(FontRole::Number11));
    QCOMPARE(dialog->findChild<QTimeEdit*>("chartAlertQuietStart")->font(),KeefiFonts::font(FontRole::Number11));
    QCOMPARE(dialog->findChild<QComboBox*>("chartAlertTimeZone")->font(),KeefiFonts::font(FontRole::Number11));
    QVERIFY(saveD5(*dialog,"alert-settings-"+theme));dialog->close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    const auto bytes=syntheticReviewPack();panel.reviewPhrasePack(bytes);auto* review=panel.findChild<QDialog*>("chartPhraseReview");QVERIFY(review);
    auto* preview=review->findChild<QTextEdit*>("chartPhraseReviewText");QVERIFY(preview);QCOMPARE(preview->font(),KeefiFonts::font(FontRole::Body15));
    const auto validation=ChartPhrasePack::validate(bytes);QStringList originalLines;for(const auto& phrase:validation.preview)originalLines<<phrase.kind+" · "+phrase.phraseId+"\n"+phrase.text;
    QCOMPARE(preview->toPlainText(),originalLines.join("\n\n"));QVERIFY(saveD5(*review,"alert-phrase-review-"+theme));
    KeefiTheme::instance()->setMode(dark?ThemeMode::Light:ThemeMode::Dark);QCoreApplication::processEvents();
    QCOMPARE(history->item(0)->foreground().color(),themeColors().danger);QCOMPARE(engine.publicState(),before);
    QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::acceptedTypedMovementRecolorsExistingHistoryAndKeepsRiskFixed_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void ChartAlertsPanelTest::acceptedTypedMovementRecolorsExistingHistoryAndKeepsRiskFixed(){
    QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    QWidget owner;MovementColorPolicy colors(&owner);Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases,&owner);owner.show();panel.show();auto* history=panel.findChild<QListWidget*>("chartAlertHistory");QVERIFY(history);
    // Deliberately misleading text proves the typed enum, rather than wording,
    // controls movement ink. Risk is independent of movement direction.
    emit engine.noticeSubmitted("typed-up","DOWN WORD","minus -123",false,MovementDirection::Up,false);
    emit engine.noticeSubmitted("typed-down","UP WORD","plus +123",false,MovementDirection::Down,false);
    emit engine.noticeSubmitted("typed-risk","Synthetic risk","Serious synthetic warning",true,MovementDirection::Unknown,true);
    QCOMPARE(history->count(),3);QCOMPARE(history->item(0)->foreground().color(),themeColors().upText);QCOMPARE(history->item(1)->foreground().color(),themeColors().downText);const auto fixed=history->item(2)->foreground().color();QCOMPARE(fixed,themeColors().danger);
    colors.setScheme(MovementScheme::KoreanRedBlue);QCoreApplication::processEvents();QCOMPARE(history->count(),3);QCOMPARE(history->item(0)->foreground().color(),themeColors().downText);QCOMPARE(history->item(1)->foreground().color(),themeColors().koreanDownText);QCOMPARE(history->item(2)->foreground().color(),fixed);QCOMPARE(sink.posts,0);QCOMPARE(sink.requests,0);
    const auto publicState=panel.publicState();
    KeefiTheme::instance()->setMode(dark?ThemeMode::Light:ThemeMode::Dark);QCoreApplication::processEvents();
    QCOMPARE(history->count(),3);QCOMPARE(history->item(0)->foreground().color(),themeColors().downText);QCOMPARE(history->item(1)->foreground().color(),themeColors().koreanDownText);QCOMPARE(history->item(2)->foreground().color(),themeColors().danger);
    QCOMPARE(panel.publicState(),publicState);QCOMPARE(sink.posts,0);QCOMPARE(sink.requests,0);
}
void ChartAlertsPanelTest::hiddenTypedHistoryCatchesUpAndPrivateMetadataNeverPersists(){
    QWidget owner;MovementColorPolicy colors(&owner);Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases,&owner);owner.show();panel.show();auto* history=panel.findChild<QListWidget*>("chartAlertHistory");QVERIFY(history);
    emit engine.noticeSubmitted("public-typed","Synthetic","Public",false,MovementDirection::Up,false);QCOMPARE(history->count(),1);panel.hide();
    emit engine.noticeSubmitted("private-typed","SYNTHETIC_PRIVATE_TYPED","Private",true,MovementDirection::Down,true);colors.setScheme(MovementScheme::KoreanRedBlue);QCOMPARE(history->count(),1);
    const auto state=panel.publicState();QVERIFY(!state.contains("SYNTHETIC_PRIVATE_TYPED"));QVERIFY(!state.contains("direction"));QVERIFY(!state.contains("fixedRisk"));panel.show();QCOMPARE(history->count(),2);panel.clearPrivate();QCOMPARE(history->count(),1);QCOMPARE(history->item(0)->foreground().color(),themeColors().downText);
}
void ChartAlertsPanelTest::horizontalIsPublicListOptInWithoutEnablingOtherKinds(){
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);QVERIFY(engine.setPublicLists({{"synthetic-lines",{{"coin","upbit","KRW-SYN"}}}}));const auto before=engine.rule("synthetic-lines");ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);panel.setListLabels({{"synthetic-lines",QStringLiteral("합성 수평선 리스트")}});panel.show();panel.openSettings("synthetic-lines");auto* dialog=panel.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);auto* horizontal=dialog->findChild<QCheckBox*>("chartAlertHorizontal");QVERIFY2(horizontal,"Public horizontal-line alerts need a list opt-in separate from indicator and drawing visibility");QVERIFY(!horizontal->isChecked());QVERIFY(horizontal->isEnabled());horizontal->setFocus();QTest::keyClick(horizontal,Qt::Key_Space);const auto after=engine.rule("synthetic-lines");QVERIFY(after.horizontal);QCOMPARE(after.enabled,before.enabled);QCOMPARE(after.pullback,before.pullback);QCOMPARE(after.results,before.results);QCOMPARE(after.whales,before.whales);QCOMPARE(after.intervalMs,before.intervalMs);QVERIFY(engine.demands().isEmpty());QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::horizontalListSwitchDoesNotWriteAndHoldingsCannotEnableIt(){
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);QVERIFY(engine.setPublicLists({{"synthetic-a",{{"coin","upbit","KRW-SYN"}}},{"synthetic-b",{{"coin","upbit","KRW-OTHER"}}}}));auto a=engine.rule("synthetic-a");a.horizontal=true;QVERIFY(engine.setRule(a));ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);panel.setListLabels({{"synthetic-a",QStringLiteral("합성 A")},{"synthetic-b",QStringLiteral("합성 B")}});panel.show();panel.openSettings("synthetic-a");auto* dialog=panel.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);auto* horizontal=dialog->findChild<QCheckBox*>("chartAlertHorizontal");auto* selector=dialog->findChild<QComboBox*>("chartAlertList");QVERIFY(horizontal);QVERIFY(selector);QVERIFY(horizontal->isChecked());QSignalSpy changed(&engine,&ChartAlertEngine::changed);selector->setCurrentIndex(selector->findData("synthetic-b"));QVERIFY(!horizontal->isChecked());QCOMPARE(changed.size(),0);QVERIFY(engine.rule("synthetic-a").horizontal);QVERIFY(!engine.rule("synthetic-b").horizontal);selector->setCurrentIndex(selector->findData("@holdings"));QVERIFY(!horizontal->isEnabled());QVERIFY(!horizontal->isChecked());QCOMPARE(changed.size(),0);QVERIFY(!engine.rule("@holdings").horizontal);QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::whaleAlertIsExplicitOptInAndKeepsTheStudyTimeframeIndependent(){
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);QVERIFY(engine.setPublicLists({{"synthetic-coin",{{"coin","upbit","KRW-SYN"}}}}));
    auto rule=engine.rule("synthetic-coin");rule.enabled=true;rule.pullback=false;rule.results=false;rule.intervalMs=900000;QVERIFY(engine.setRule(rule));
    ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);panel.setListLabels({{"synthetic-coin",QStringLiteral("합성 코인 리스트")}});panel.show();panel.openSettings("synthetic-coin");
    auto* dialog=panel.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);auto* whales=dialog->findChild<QCheckBox*>("chartAlertWhales");
    QVERIFY2(whales,"Large-trade and reversal alerts need an explicit list-level opt-in separate from indicator visibility");
    QVERIFY(!whales->isChecked());QVERIFY(whales->text().contains(QStringLiteral("완료 1분")));
    whales->setFocus();QTest::keyClick(whales,Qt::Key_Space);QVERIFY(engine.rule("synthetic-coin").whales);QCOMPARE(engine.rule("synthetic-coin").intervalMs,qint64(900000));
    const auto demand=engine.demands();QCOMPARE(demand.size(),1);QCOMPARE(demand.first().intervalMs,qint64(60000));QVERIFY(demand.first().whales);
    auto* timeframe=dialog->findChild<QComboBox*>("chartAlertTimeframe");QVERIFY(timeframe);timeframe->setCurrentIndex(timeframe->findData(qint64(86400000)));
    QCOMPARE(engine.rule("synthetic-coin").intervalMs,qint64(86400000));QCOMPARE(engine.demands().first().intervalMs,qint64(60000));
    whales->setFocus();QTest::keyClick(whales,Qt::Key_Space);QVERIFY(!engine.rule("synthetic-coin").whales);QVERIFY(engine.demands().isEmpty());
    QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::dripDefaultCanBeDisabledImmediatelyWithoutSystemEffects(){
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);engine.setPhrasePack(&phrases);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);
    panel.show();auto* drip=panel.findChild<QCheckBox*>("chartDripEnabled");QVERIFY(drip);QVERIFY(drip->isChecked());
    QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);QTest::mouseClick(drip,Qt::LeftButton);QVERIFY(!engine.settings().dripEnabled);
    QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::permissionRequiresSettingsActionAndIntroIsRemembered(){
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);
    panel.show();panel.openSettings("@holdings");auto* dialog=panel.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);
    auto* intro=dialog->findChild<QLabel*>("chartDripIntroduction");QVERIFY(intro);QVERIFY(intro->isVisible());
    auto* allowed=dialog->findChild<QPushButton*>("chartNotificationAuthorize");QVERIFY(allowed);QCOMPARE(sink.requests,0);QTest::mouseClick(allowed,Qt::LeftButton);QCOMPARE(sink.requests,1);
    auto* enabled=dialog->findChild<QCheckBox*>("chartListAlertsEnabled");QVERIFY(enabled);QVERIFY(enabled->isChecked());dialog->close();
    const auto state=panel.publicState();QVERIFY(!state.isEmpty());
    ChartAlertsPanel restored(&engine,nullptr,&sink,&phrases);QVERIFY(restored.restorePublicState(state));restored.show();restored.openSettings("@holdings");
    dialog=restored.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);intro=dialog->findChild<QLabel*>("chartDripIntroduction");QVERIFY(!intro||!intro->isVisible());QCOMPARE(sink.requests,1);
}
void ChartAlertsPanelTest::privateHistoryIsErasedAndHiddenUpdatesCatchUpOnShow(){
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);panel.show();
    auto* history=panel.findChild<QListWidget*>("chartAlertHistory");QVERIFY(history);
    emit engine.noticeSubmitted("public-1","합성 공개","합성 공개 완료 신호",false);
    emit engine.noticeSubmitted("private-1","합성 보유","SYNTHETIC_PRIVATE_BODY",true);QCOMPARE(history->count(),2);
    panel.clearPrivate();QCOMPARE(history->count(),1);QVERIFY(!history->item(0)->text().contains("SYNTHETIC_PRIVATE_BODY"));
    panel.hide();emit engine.noticeSubmitted("public-2","합성 공개2","합성 다음 완료 신호",false);QCOMPARE(history->count(),1);
    panel.show();QCOMPARE(history->count(),2);
}
void ChartAlertsPanelTest::uiPreferencesContainNoHistoryAndRejectUnknownFields(){
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);
    QVERIFY(panel.backgroundEnabled());QVERIFY(!panel.closeHintSeen());panel.setBackgroundEnabled(false);panel.markCloseHintSeen();
    const auto state=panel.publicState();QVERIFY(!state.isEmpty());QVERIFY(!state.contains("history"));
    ChartAlertsPanel restored(&engine,nullptr,&sink,&phrases);QVERIFY(restored.restorePublicState(state));QVERIFY(!restored.backgroundEnabled());QVERIFY(restored.closeHintSeen());
    const auto before=restored.publicState();QVERIFY(!restored.restorePublicState("{\"privateBody\":\"SYNTHETIC_PRIVATE_BODY\"}"));QCOMPARE(restored.publicState(),before);
}
void ChartAlertsPanelTest::phrasePackRequiresHumanReviewAndInvalidPackDoesNotApply(){
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);panel.show();
    const auto bytes=syntheticReviewPack();const auto validation=ChartPhrasePack::validate(bytes);QVERIFY(validation.valid);
    panel.reviewPhrasePack(bytes);QPointer<QDialog> review=panel.findChild<QDialog*>("chartPhraseReview");QVERIFY(review);
    auto* checked=review->findChild<QCheckBox*>("chartPhraseReviewed");auto* apply=review->findChild<QPushButton*>("chartPhraseApply");
    QVERIFY(checked);QVERIFY(apply);QVERIFY(!checked->isChecked());QVERIFY(!apply->isEnabled());
    QVERIFY(review->findChild<QTextEdit*>("chartPhraseReviewText")->toPlainText().contains("{sym}"));
    QTest::mouseClick(apply,Qt::LeftButton);QVERIFY(phrases.importedSha256().isEmpty());
    QStyleOptionButton option;option.initFrom(checked);option.text=checked->text();
    const auto clickable=checked->style()->subElementRect(QStyle::SE_CheckBoxClickRect,&option,checked);
    const auto indicator=checked->style()->subElementRect(QStyle::SE_CheckBoxIndicator,&option,checked);
    qInfo()<<"synthetic review checkbox geometry"<<checked->rect()<<"clickable"<<clickable<<"default-center-inside"<<clickable.contains(checked->rect().center());
    QTest::mouseClick(checked,Qt::LeftButton,Qt::NoModifier,indicator.center());QVERIFY(checked->isChecked());QVERIFY(apply->isEnabled());QTest::mouseClick(apply,Qt::LeftButton);
    QTRY_COMPARE(phrases.importedSha256(),validation.sha256);QTRY_VERIFY(review.isNull());
    const auto accepted=panel.publicState();
    panel.reviewPhrasePack("{\"privateBody\":\"SYNTHETIC_PRIVATE_SENTINEL\"}");review=panel.findChild<QDialog*>("chartPhraseReview");QVERIFY(review);
    checked=review->findChild<QCheckBox*>("chartPhraseReviewed");apply=review->findChild<QPushButton*>("chartPhraseApply");
    QVERIFY(!checked->isEnabled());QVERIFY(!apply->isEnabled());QTest::mouseClick(apply,Qt::LeftButton);
    QCOMPARE(phrases.importedSha256(),validation.sha256);QCOMPARE(panel.publicState(),accepted);
    QVERIFY(!panel.publicState().contains("SYNTHETIC_PRIVATE_SENTINEL"));QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::reviewedPublicPackShaRoundTripsThroughTemporaryProfile(){
    QTemporaryDir directory;QVERIFY(directory.isValid());
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);panel.show();
    const auto bytes=syntheticReviewPack();const auto validation=ChartPhrasePack::validate(bytes);QVERIFY(validation.valid);
    panel.reviewPhrasePack(bytes);auto* review=panel.findChild<QDialog*>("chartPhraseReview");QVERIFY(review);
    auto* reviewed=review->findChild<QCheckBox*>("chartPhraseReviewed");QVERIFY(reviewed);reviewed->setFocus();QTest::keyClick(reviewed,Qt::Key_Space);QVERIFY(reviewed->isChecked());
    auto* apply=review->findChild<QPushButton*>("chartPhraseApply");QVERIFY(apply);QVERIFY(apply->isEnabled());QTest::mouseClick(apply,Qt::LeftButton);
    QCOMPARE(phrases.importedSha256(),validation.sha256);const auto state=panel.publicState();const auto object=QJsonDocument::fromJson(state).object();
    QCOMPARE(object.size(),6);QCOMPARE(object.value("reviewedSha256").toString().toLatin1(),validation.sha256);
    QCOMPARE(QByteArray::fromBase64(object.value("reviewedPack").toString().toLatin1()),bytes);QVERIFY(!state.contains("history"));
    const auto path=directory.filePath("synthetic-alert-profile.ini");
    {QSettings settings(path,QSettings::IniFormat);settings.setValue("chart/alerts/ui/v1",state);settings.sync();QCOMPARE(settings.status(),QSettings::NoError);}
    ChartPhrasePack restoredPhrases;ChartAlertEngine restoredEngine(&sink);ChartAlertsPanel restored(&restoredEngine,nullptr,&sink,&restoredPhrases);
    QSettings saved(path,QSettings::IniFormat);QVERIFY(restored.restorePublicState(saved.value("chart/alerts/ui/v1").toByteArray()));
    QCOMPARE(restoredPhrases.importedSha256(),validation.sha256);QCOMPARE(restored.publicState(),state);
    auto corrupt=object;corrupt["reviewedSha256"]=QString(64,'0');
    QVERIFY(!restored.restorePublicState(QJsonDocument(corrupt).toJson(QJsonDocument::Compact)));QCOMPARE(restored.publicState(),state);QCOMPARE(restoredPhrases.importedSha256(),validation.sha256);
    auto changed=QJsonDocument::fromJson(bytes).object();changed["name"]=QStringLiteral("수정된 합성 팩");
    corrupt=object;corrupt["reviewedPack"]=QString::fromLatin1(QJsonDocument(changed).toJson(QJsonDocument::Compact).toBase64());
    QVERIFY(!restored.restorePublicState(QJsonDocument(corrupt).toJson(QJsonDocument::Compact)));QCOMPARE(restored.publicState(),state);QCOMPARE(restoredPhrases.importedSha256(),validation.sha256);
    restored.show();restored.openSettings();auto* dialog=restored.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);
    QTest::mouseClick(dialog->findChild<QPushButton*>("chartPhraseUsePlain"),Qt::LeftButton);QVERIFY(restoredPhrases.importedSha256().isEmpty());
    QVERIFY(QJsonDocument::fromJson(restored.publicState()).object().value("reviewedPack").toString().isEmpty());QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::listRulesQuietHoursAndDailyCapEditTheInjectedEngine(){
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);
    QVERIFY(engine.setPublicLists({{"synthetic-public",{{"coin","upbit","KRW-SYN"}}}}));
    ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);panel.setListLabels({{"synthetic-public",QStringLiteral("합성 공개 리스트")}});panel.show();panel.openSettings("synthetic-public");
    auto* dialog=panel.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);
    auto* enabled=dialog->findChild<QCheckBox*>("chartListAlertsEnabled");auto* liquidation=dialog->findChild<QCheckBox*>("chartAlertLiquidation");
    QVERIFY(enabled);QVERIFY(!enabled->isChecked());QVERIFY(liquidation);QVERIFY(!liquidation->isEnabled());
    enabled->setChecked(true);dialog->findChild<QCheckBox*>("chartAlertPullback")->setChecked(false);
    auto* timeframe=dialog->findChild<QComboBox*>("chartAlertTimeframe");timeframe->setCurrentIndex(timeframe->findData(qint64(300000)));
    dialog->findChild<QSpinBox*>("chartAlertMinimumScore")->setValue(8);dialog->findChild<QSpinBox*>("chartAlertCooldown")->setValue(9);
    const auto rule=engine.rule("synthetic-public");QVERIFY(rule.enabled);QVERIFY(!rule.pullback);QVERIFY(rule.results);QVERIFY(!rule.liquidation);
    QCOMPARE(rule.intervalMs,qint64(300000));QCOMPARE(rule.minimumScore,8);QCOMPARE(rule.cooldownMs,qint64(540000));
    QVERIFY(engine.rule("@holdings").enabled);QCOMPARE(engine.rule("@holdings").intervalMs,qint64(900000));
    auto* selector=dialog->findChild<QComboBox*>("chartAlertList");selector->setCurrentIndex(selector->findData("@holdings"));QVERIFY(liquidation->isEnabled());liquidation->setChecked(true);
    dialog->findChild<QDoubleSpinBox*>("chartAlertDistance")->setValue(2.5);QVERIFY(engine.rule("@holdings").liquidation);QCOMPARE(engine.rule("@holdings").liquidationDistancePercent,2.5);
    dialog->findChild<QCheckBox*>("chartAlertQuietEnabled")->setChecked(true);dialog->findChild<QTimeEdit*>("chartAlertQuietStart")->setTime(QTime(23,15));dialog->findChild<QTimeEdit*>("chartAlertQuietEnd")->setTime(QTime(6,45));
    auto* zone=dialog->findChild<QComboBox*>("chartAlertTimeZone");zone->setCurrentIndex(zone->findData(QByteArray("UTC")));dialog->findChild<QSpinBox*>("chartAlertDailyLimit")->setValue(7);
    dialog->findChild<QCheckBox*>("chartAlertSettingsDrip")->setChecked(false);
    const auto settings=engine.settings();QVERIFY(settings.quietEnabled);QCOMPARE(settings.quietStartMinute,1395);QCOMPARE(settings.quietEndMinute,405);QCOMPARE(settings.timeZoneId,QByteArray("UTC"));QCOMPARE(settings.dailyLimit,7);QVERIFY(!settings.dripEnabled);
    QVERIFY(!panel.findChild<QCheckBox*>("chartDripEnabled")->isChecked());QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
    ChartAlertEngine restored(&sink);QVERIFY(restored.setPublicLists({{"synthetic-public",{{"coin","upbit","KRW-SYN"}}}}));
    QVERIFY(restored.restorePublicState(engine.publicState()));QCOMPARE(restored.rule("synthetic-public").minimumScore,8);QCOMPARE(restored.settings().dailyLimit,7);
}
void ChartAlertsPanelTest::unavailableBackgroundDisablesControlAndExplainsCloseQuits(){
    Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases);panel.show();panel.openSettings();
    auto* dialog=panel.findChild<QDialog*>("chartAlertSettingsDialog");QVERIFY(dialog);
    QVERIFY2(!dialog->findChild<QCheckBox*>("chartAlertBackgroundEnabled"),"Background residency is edited only in Options > General");
    auto* state=dialog->findChild<QLabel*>("chartAlertBackgroundStatus");auto* general=dialog->findChild<QPushButton*>("chartAlertOpenGeneral");QVERIFY(state);QVERIFY(general);
    QSignalSpy route(&panel,&ChartAlertsPanel::generalSettingsRequested);
    QVERIFY(panel.backgroundEnabled());QVERIFY(state->text().contains(QStringLiteral("메뉴 막대")));
    panel.setBackgroundAvailable(false);QVERIFY(state->text().contains(QStringLiteral("창을 닫으면")));QVERIFY(state->text().contains(QStringLiteral("종료")));
    QVERIFY(QJsonDocument::fromJson(panel.publicState()).object().value("backgroundEnabled").toBool());
    general->click();QCOMPARE(route.size(),1);QVERIFY(panel.backgroundEnabled());
    panel.setBackgroundEnabled(false);panel.setBackgroundAvailable(true);QVERIFY(state->text().contains(QStringLiteral("종료")));
    panel.setBackgroundEnabled(true);QVERIFY(state->text().contains(QStringLiteral("메뉴 막대")));
    QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::embeddedPageSurvivesOptionRecreationAndRetargetsRules(){
    QWidget appOwner;Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);
    QVERIFY(engine.setPublicLists({{"synthetic-a",{{"coin","upbit","KRW-SYN"}}},{"synthetic-b",{{"coin","upbit","KRW-OTHER"}}}}));
    ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases,&appOwner);panel.hide();
    panel.setListLabels({{"synthetic-a",QStringLiteral("합성 A")},{"synthetic-b",QStringLiteral("합성 B")}});
    QPointer<QWidget> firstPage;QByteArray engineState,uiState;
    {
        QDialog options(&appOwner);auto* layout=new QVBoxLayout(&options);firstPage=panel.createSettingsPage(&options,"synthetic-a");layout->addWidget(firstPage);options.resize(620,720);options.show();QCoreApplication::processEvents();
        QVERIFY(firstPage->isVisible());QVERIFY(!panel.isVisible());
        auto* selector=firstPage->findChild<QComboBox*>("chartAlertList");QVERIFY(selector);QCOMPARE(selector->currentData().toString(),QString("synthetic-a"));
        firstPage->findChild<QCheckBox*>("chartListAlertsEnabled")->setChecked(true);
        firstPage->findChild<QCheckBox*>("chartAlertSettingsDrip")->setChecked(false);
        firstPage->findChild<QCheckBox*>("chartObservationAlert_strong_reversal")->setChecked(true);
        emit engine.noticeSubmitted("embedded-first","합성 첫 알림","완료봉 15분",false);
        engineState=engine.publicState();uiState=panel.publicState();
        QVERIFY(engine.rule("synthetic-a").enabled);QVERIFY(!engine.settings().dripEnabled);QVERIFY(engine.observationEnabled("strong_reversal"));
    }
    QVERIFY(firstPage.isNull());QCOMPARE(engine.publicState(),engineState);QCOMPARE(panel.publicState(),uiState);
    emit engine.noticeSubmitted("embedded-closed","합성 닫힌 후 알림","메모리 이력 유지",false);
    QDialog options(&appOwner);auto* layout=new QVBoxLayout(&options);auto* page=panel.createSettingsPage(&options,"synthetic-b");layout->addWidget(page);options.resize(620,720);options.show();QCoreApplication::processEvents();
    auto* selector=page->findChild<QComboBox*>("chartAlertList");QCOMPARE(selector->currentData().toString(),QString("synthetic-b"));
    QVERIFY(!page->findChild<QCheckBox*>("chartAlertSettingsDrip")->isChecked());
    QVERIFY(page->findChild<QCheckBox*>("chartObservationAlert_strong_reversal")->isChecked());
    auto* recent=page->findChild<QLabel*>("chartAlertHistoryRow_0");QVERIFY(recent);QVERIFY(recent->text().contains(QStringLiteral("합성 닫힌 후 알림")));
    QVERIFY(page->findChild<QLabel*>("chartAlertHistoryRow_1")->text().contains(QStringLiteral("합성 첫 알림")));
    QSignalSpy changed(&engine,&ChartAlertEngine::changed);panel.selectSettingsList("synthetic-a");
    QCOMPARE(selector->currentData().toString(),QString("synthetic-a"));QVERIFY(page->findChild<QCheckBox*>("chartListAlertsEnabled")->isChecked());QCOMPARE(changed.size(),0);
    panel.setListLabels({{"synthetic-a",QStringLiteral("합성 이름 변경")},{"synthetic-b",QStringLiteral("합성 B")}});
    QCOMPARE(selector->currentData().toString(),QString("synthetic-a"));QCOMPARE(selector->currentText(),QStringLiteral("합성 이름 변경"));QCOMPARE(changed.size(),0);
    engine.setDripEnabled(true);QVERIFY(page->findChild<QCheckBox*>("chartAlertSettingsDrip")->isChecked());
    panel.show();panel.hide();QVERIFY(page->isVisible());QVERIFY(engine.rule("synthetic-a").enabled);
    QVERIFY(!panel.publicState().contains("embedded-closed"));QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::hiddenControllerUsesStaticBoundedHistoryAndErasesAccessiblePrivateText(){
    QWidget appOwner;MovementColorPolicy colors(&appOwner);Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);
    ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases,&appOwner);panel.hide();
    QDialog options(&appOwner);auto* layout=new QVBoxLayout(&options);auto* page=panel.createSettingsPage(&options);layout->addWidget(page);options.resize(620,720);options.show();QCoreApplication::processEvents();
    auto* history=page->findChild<QWidget*>("chartAlertRecentHistory");QVERIFY(history);QVERIFY(history->findChildren<QAbstractItemView*>().isEmpty());
    QList<QLabel*> rows;for(int i=0;i<30;++i){auto* row=history->findChild<QLabel*>("chartAlertHistoryRow_"+QString::number(i));QVERIFY(row);rows.append(row);}
    auto* legacy=panel.findChild<QListWidget*>("chartAlertHistory");QVERIFY(legacy);QCOMPARE(legacy->count(),0);
    emit engine.noticeSubmitted("only-private","SYNTHETIC_PRIVATE_TITLE","SYNTHETIC_PRIVATE_BODY",true,MovementDirection::Unknown,true);
    QVERIFY(rows.first()->accessibleName().contains("SYNTHETIC_PRIVATE_BODY"));panel.clearPrivate();
    for(auto* row:rows){QVERIFY(row->text().isEmpty());QVERIFY(row->accessibleName().isEmpty());QVERIFY(row->toolTip().isEmpty());QVERIFY(!row->property("noticeId").isValid());}
    for(int i=0;i<35;++i)emit engine.noticeSubmitted("bounded-"+QString::number(i),"합성 "+QString::number(i),"완료봉 원문 <tag> 12:05",false,MovementDirection::Up,false);
    emit engine.noticeSubmitted("bounded-34","DUPLICATE MUST NOT APPEAR","duplicate",false);
    QCOMPARE(rows.first()->property("noticeId").toString(),QString("bounded-34"));QCOMPARE(rows.last()->property("noticeId").toString(),QString("bounded-5"));
    QCOMPARE(legacy->count(),0);
    for(int i=0;i<30;++i){QCOMPARE(history->findChild<QLabel*>("chartAlertHistoryRow_"+QString::number(i)),rows[i]);QVERIFY(!rows[i]->isHidden());QVERIFY(rows[i]->text().contains("원문 <tag>"));}
    const auto originalColor=rows.first()->styleSheet();colors.setScheme(MovementScheme::KoreanRedBlue);QVERIFY(rows.first()->styleSheet()!=originalColor);
    emit engine.noticeSubmitted("fixed-risk","합성 청산 경고","항상 진지한 경고",true,MovementDirection::Down,true);
    QVERIFY(rows.first()->styleSheet().contains(themeColors().danger.name()));QVERIFY(rows.first()->property("fixedRisk").toBool());
    panel.clearPrivate();QVERIFY(!rows.first()->text().contains(QStringLiteral("청산 경고")));QVERIFY(rows.last()->text().isEmpty());QVERIFY(rows.last()->accessibleName().isEmpty());
    QVERIFY(!panel.publicState().contains("SYNTHETIC_PRIVATE"));QVERIFY(!panel.publicState().contains("bounded-"));QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::unshownPageDoesNotConsumeIntroOrRequestPermission(){
    QWidget appOwner;Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases,&appOwner);panel.hide();
    {
        QDialog unopened(&appOwner);auto* layout=new QVBoxLayout(&unopened);layout->addWidget(panel.createSettingsPage(&unopened));
        QVERIFY(!QJsonDocument::fromJson(panel.publicState()).object().value("dripIntroSeen").toBool());QCOMPARE(sink.requests,0);
    }
    {
        QDialog options(&appOwner);auto* layout=new QVBoxLayout(&options);auto* page=panel.createSettingsPage(&options);layout->addWidget(page);options.show();QCoreApplication::processEvents();
        QVERIFY(QJsonDocument::fromJson(panel.publicState()).object().value("dripIntroSeen").toBool());QVERIFY(page->findChild<QLabel*>("chartDripIntroduction")->isVisible());
        QVERIFY(page->findChild<QScrollArea*>()==nullptr);QCOMPARE(sink.requests,0);
    }
    QDialog reopened(&appOwner);auto* layout=new QVBoxLayout(&reopened);auto* page=panel.createSettingsPage(&reopened);layout->addWidget(page);reopened.show();QCoreApplication::processEvents();
    QVERIFY(!page->findChild<QLabel*>("chartDripIntroduction")->isVisible());QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::settingsEntrypointRoutesAndEmbeddedReviewFollowsOptions(){
    QWidget appOwner;Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);ChartAlertsPanel panel(&engine,nullptr,&sink,&phrases,&appOwner);panel.hide();
    QSignalSpy route(&panel,&ChartAlertsPanel::settingsRequested);panel.openSettings("synthetic-context");
    QCOMPARE(route.size(),1);QCOMPARE(route.first().first().toString(),QString("synthetic-context"));QVERIFY(!panel.findChild<QDialog*>("chartAlertSettingsDialog"));
    QPointer<QDialog> review;const auto before=panel.publicState();
    {
        QDialog options(&appOwner);auto* layout=new QVBoxLayout(&options);auto* page=panel.createSettingsPage(&options);layout->addWidget(page);options.show();QCoreApplication::processEvents();
        panel.reviewPhrasePack(syntheticReviewPack());review=page->findChild<QDialog*>("chartPhraseReview");QVERIFY(review);QVERIFY(review->isVisible());
        QVERIFY(!review->findChild<QPushButton*>("chartPhraseApply")->isEnabled());panel.show();panel.hide();QVERIFY(review->isVisible());QVERIFY(page->isVisible());
    }
    QVERIFY(review.isNull());QVERIFY(phrases.importedSha256().isEmpty());
    const auto state=QJsonDocument::fromJson(panel.publicState()).object();QVERIFY(state.value("reviewedPack").toString().isEmpty());
    QCOMPARE(state.value("backgroundEnabled"),QJsonDocument::fromJson(before).object().value("backgroundEnabled"));QCOMPARE(sink.requests,0);QCOMPARE(sink.posts,0);
}
void ChartAlertsPanelTest::deletingControllerRevokesDisposablePageCallbacks(){
    QWidget appOwner;Sink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink);
    auto* panel=new ChartAlertsPanel(&engine,nullptr,&sink,&phrases,&appOwner);panel->hide();
    QDialog options(&appOwner);auto* layout=new QVBoxLayout(&options);QPointer<QWidget> page=panel->createSettingsPage(&options);layout->addWidget(page);options.show();QCoreApplication::processEvents();
    QVERIFY(page);delete panel;QVERIFY(page.isNull());QVERIFY(options.isVisible());
    // The source can outlive the UI owner. No page callback may capture the
    // destroyed controller, and deleting the options later must not double-delete.
    engine.setDripEnabled(false);emit engine.noticeSubmitted("after-owner","Synthetic","Owner gone",false);QCoreApplication::processEvents();
    QCOMPARE(sink.posts,0);QCOMPARE(sink.requests,0);
}
QTEST_MAIN(ChartAlertsPanelTest)
#include "tst_chart_alerts_panel.moc"
