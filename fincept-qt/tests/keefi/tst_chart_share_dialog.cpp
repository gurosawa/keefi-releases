#include "keefi/ChartShareDialog.h"
#include "keefi/MarketCandles.h"
#include "keefi/KeefiTheme.h"
#include <QCheckBox>
#include <QDir>
#include <QDataStream>
#include <QFile>
#include <QImageReader>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
using namespace keefi;
namespace {
ChartShareSnapshot syntheticSnapshot(){
    constexpr qint64 start=1792713600000,minute=60000;ChartShareSnapshot value;value.identity={"coin","upbit","KRW-SYN"};value.currency="KRW";value.intervalMs=minute;value.displayTimeZoneId="Asia/Seoul";
    for(int i=0;i<30;++i)value.closed.append({start+i*minute,start+(i+1)*minute-1,"100","102","99",i%2?"101":"100","2"});
    value.calculatedThroughMs=start+30*minute;value.asOfMs=value.calculatedThroughMs+1000;value.viewport={start,start+29*minute};value.studies=keefiChartStudies(value.closed,minute);
    value.source.provider=ChartShareSource::Upbit;value.source.canonicalUrl=MarketCandles::request(value.identity.key(),minute).url.toString();value.source.receivedAtMs=value.calculatedThroughMs+500;
    value.visiblePublicLayers={"pullback","cloud","volume"};value.availability=ChartShareAvailability::Ready;return value;
}
QByteArray publicSnapshotInputs(const ChartShareSnapshot& value){
    QByteArray bytes;QDataStream out(&bytes,QIODevice::WriteOnly);
    out<<value.identity.key()<<value.currency<<value.intervalMs<<value.asOfMs<<value.displayTimeZoneId
       <<value.calculatedThroughMs<<value.viewport.fromMs<<value.viewport.toMs<<value.visiblePublicLayers
       <<int(value.source.provider)<<value.source.canonicalUrl<<value.source.sourceTimeMs<<value.source.receivedAtMs
       <<value.source.delayed<<int(value.source.adjustment)<<int(value.colors)<<int(value.availability)<<bool(value.stockTimeline);
    for(const auto& row:value.closed)out<<row.startsAtMs<<row.lastStoredTickMs<<row.open<<row.high<<row.low<<row.close<<row.volume;
    for(const auto& drawing:value.drawings){out<<int(drawing.kind);for(const auto& point:drawing.anchors)out<<point.atMs<<point.price;}
    return bytes;
}
QByteArray read(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
}
class ChartShareDialogTest final:public QObject {
    Q_OBJECT
  private slots:
    void previewCopiesOnlyTheSuppliedPublicSnapshotAndKeepsMaskFixed();
    void canceledSaveHasNoWritesAndNoAutomaticOutput();
    void explicitPngSaveMatchesPreviewAfterCallerMutation();
    void invalidSnapshotCannotOpenAChooser_data();
    void invalidSnapshotCannotOpenAChooser();
    void publicDrawingsNeedExplicitPreviewOptIn();
    void chooserCanDestroyTheDialogWithoutWriting();
    void themeChangesOnlyPreviewInkAndPreservesPublicInputs();
    void themeChangePreservesSaveStatusAndExistingFiles_data();
    void themeChangePreservesSaveStatusAndExistingFiles();
    void themeChangeInsideChooserDoesNotTriggerActions();
    void shareControlsHave44PixelTargets_data();
    void shareControlsHave44PixelTargets();
    void publicDrawingsToggleFromTheRowEdgeWithoutSaving();
};
void ChartShareDialogTest::previewCopiesOnlyTheSuppliedPublicSnapshotAndKeepsMaskFixed(){
    auto value=syntheticSnapshot();ChartShareDialog dialog(value);value.identity.code="CHANGED_OUTSIDE";value.closed.clear();dialog.show();
    QCOMPARE(dialog.snapshot().identity.code,QString("KRW-SYN"));QCOMPARE(dialog.snapshot().closed.size(),30);QVERIFY(dialog.options().maskPersonalValues);QVERIFY(!dialog.options().includePublicDrawings);
    auto* privacy=dialog.findChild<QLabel*>("chartSharePrivacy");auto* save=dialog.findChild<QPushButton*>("chartShareSave");QVERIFY(privacy&&save);QVERIFY(privacy->text().contains(QStringLiteral("계좌 금액·수량 제외")));QVERIFY(privacy->text().contains(QStringLiteral("로컬 저장")));
    QCOMPARE(dialog.findChildren<QCheckBox*>().size(),1);QVERIFY(dialog.previewAvailable());QVERIFY(save->isEnabled());QCOMPARE(dialog.previewImage().size(),QSize(1200,900));QVERIFY(dialog.previewImage().textKeys().isEmpty());
    auto detached=dialog.snapshot();detached.currency="CHANGED";QCOMPARE(dialog.snapshot().currency,QString("KRW"));const auto image=dialog.previewImage();dialog.resize(720,640);QCOMPARE(dialog.previewImage(),image);
}
void ChartShareDialogTest::canceledSaveHasNoWritesAndNoAutomaticOutput(){
    QTemporaryDir directory;QVERIFY(directory.isValid());const auto sentinel=directory.filePath("untouched.png");{QFile file(sentinel);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write("SYNTHETIC_ORIGINAL"),qint64(18));}
    ChartShareDialog dialog(syntheticSnapshot());int choices=0;dialog.setPathChooser([&](QWidget*){++choices;return QString{};});QSignalSpy saved(&dialog,&ChartShareDialog::saved);dialog.show();dialog.resize(780,680);
    QCOMPARE(choices,0);QCOMPARE(QDir(directory.path()).entryList(QDir::Files),QStringList{"untouched.png"});QCOMPARE(read(sentinel),QByteArray("SYNTHETIC_ORIGINAL"));
    auto* save=dialog.findChild<QPushButton*>("chartShareSave");QVERIFY(save);save->click();QCOMPARE(choices,1);QCOMPARE(saved.size(),0);QCOMPARE(read(sentinel),QByteArray("SYNTHETIC_ORIGINAL"));dialog.reject();QCOMPARE(choices,1);QCOMPARE(QDir(directory.path()).entryList(QDir::Files),QStringList{"untouched.png"});
}
void ChartShareDialogTest::explicitPngSaveMatchesPreviewAfterCallerMutation(){
    QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath("synthetic-card.png");auto input=syntheticSnapshot();ChartShareDialog dialog(input);input.closed.last().close="999";input.studies={};
    dialog.setPathChooser([path](QWidget*){return path;});QSignalSpy saved(&dialog,&ChartShareDialog::saved);dialog.show();auto* save=dialog.findChild<QPushButton*>("chartShareSave");QVERIFY(save);save->click();
    QCOMPARE(saved.size(),1);QCOMPARE(saved.first().first().toString(),path);QVERIFY(QFile::exists(path));QImageReader reader(path,"png");QVERIFY(reader.canRead());QVERIFY(reader.textKeys().isEmpty());
    // PNG decode changes Qt's storage format, not the exported visible pixels.
    QCOMPARE(reader.read().convertToFormat(QImage::Format_ARGB32),dialog.previewImage().convertToFormat(QImage::Format_ARGB32));QCOMPARE(dialog.snapshot().closed.last().close,QString("101"));
}
void ChartShareDialogTest::invalidSnapshotCannotOpenAChooser_data(){QTest::addColumn<QString>("fault");QTest::newRow("unmask")<<QString("unmask");QTest::newRow("private-source")<<QString("private-source");}
void ChartShareDialogTest::invalidSnapshotCannotOpenAChooser(){
    QFETCH(QString,fault);auto input=syntheticSnapshot();ChartShareOptions options;if(fault=="unmask")options.maskPersonalValues=false;else input.source.canonicalUrl="https://api.upbit.com/v1/accounts?SYNTHETIC_PRIVATE_CANARY";
    ChartShareDialog dialog(input,options);int choices=0;dialog.setPathChooser([&](QWidget*){++choices;return QString("/never-written.png");});dialog.show();QVERIFY(!dialog.previewAvailable());QVERIFY(dialog.previewImage().isNull());auto* save=dialog.findChild<QPushButton*>("chartShareSave");QVERIFY(save);QVERIFY(!save->isEnabled());save->click();QCOMPARE(choices,0);
    for(const auto* label:dialog.findChildren<QLabel*>())QVERIFY(!label->text().contains("SYNTHETIC_PRIVATE_CANARY"));
}
void ChartShareDialogTest::publicDrawingsNeedExplicitPreviewOptIn(){
    auto input=syntheticSnapshot();input.drawings={{ChartDrawingKind::Horizontal,{{input.closed.first().startsAtMs,"100.5"}}}};ChartShareDialog dialog(input);dialog.show();auto* include=dialog.findChild<QCheckBox*>("chartShareDrawings");QVERIFY(include);QVERIFY(include->isEnabled());QVERIFY(!include->isChecked());QVERIFY(!dialog.options().includePublicDrawings);const auto before=dialog.previewImage();
    include->setChecked(true);QVERIFY(dialog.options().includePublicDrawings);QVERIFY(dialog.previewAvailable());QVERIFY(dialog.previewImage()!=before);include->setChecked(false);QVERIFY(!dialog.options().includePublicDrawings);QCOMPARE(dialog.previewImage(),before);
}
void ChartShareDialogTest::chooserCanDestroyTheDialogWithoutWriting(){
    QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath("never-written.png");QPointer<ChartShareDialog> dialog=new ChartShareDialog(syntheticSnapshot());dialog->setPathChooser([&](QWidget*){delete dialog.data();return path;});dialog->show();auto* save=dialog->findChild<QPushButton*>("chartShareSave");QVERIFY(save);save->click();const bool destroyed=dialog.isNull();if(dialog)delete dialog.data();QVERIFY(destroyed);QVERIFY(!QFile::exists(path));
}
void ChartShareDialogTest::themeChangesOnlyPreviewInkAndPreservesPublicInputs(){
    auto* theme=KeefiTheme::instance();const auto oldMode=theme->mode();const auto reset=qScopeGuard([=]{theme->setMode(oldMode);});theme->setMode(ThemeMode::Light);
    QTemporaryDir directory;QVERIFY(directory.isValid());const auto sentinel=directory.filePath("untouched.png");
    {QFile file(sentinel);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write("SYNTHETIC_UNCHANGED"),qint64(19));}
    auto input=syntheticSnapshot();input.drawings={{ChartDrawingKind::Horizontal,{{input.closed.first().startsAtMs,"100.5"}}}};
    ChartShareOptions options;options.includePublicDrawings=true;ChartShareDialog dialog(input,options);int choices=0;
    dialog.setPathChooser([&](QWidget*){++choices;return directory.filePath("unexpected.png");});QSignalSpy saved(&dialog,&ChartShareDialog::saved),failed(&dialog,&ChartShareDialog::saveFailed);dialog.show();
    auto* preview=dialog.findChild<QLabel*>("chartSharePreview");auto* status=dialog.findChild<QLabel*>("chartShareStatus");auto* drawings=dialog.findChild<QCheckBox*>("chartShareDrawings");auto* save=dialog.findChild<QPushButton*>("chartShareSave");QVERIFY(preview&&status&&drawings&&save);
    const auto light=dialog.previewImage();const auto facts=publicSnapshotInputs(dialog.snapshot());const auto captions=preview->accessibleDescription();const auto statusText=status->text();const auto original=read(sentinel);
    QVERIFY(dialog.previewAvailable());QCOMPARE(light.pixelColor(0,0),KeefiTheme::colorsFor(false).bg);QVERIFY(drawings->isChecked());
    theme->setMode(ThemeMode::Dark);
    QCOMPARE(dialog.previewImage().pixelColor(0,0),KeefiTheme::colorsFor(true).bg);QVERIFY(dialog.previewImage()!=light);
    QCOMPARE(publicSnapshotInputs(dialog.snapshot()),facts);QCOMPARE(preview->accessibleDescription(),captions);QCOMPARE(status->text(),statusText);
    QCOMPARE(dialog.options().outputSize,options.outputSize);QCOMPARE(dialog.options().maskPersonalValues,options.maskPersonalValues);QCOMPARE(dialog.options().includePublicDrawings,options.includePublicDrawings);
    QVERIFY(drawings->isChecked());QVERIFY(drawings->isEnabled());QVERIFY(save->isEnabled());QCOMPARE(choices,0);QCOMPARE(saved.size(),0);QCOMPARE(failed.size(),0);
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files),QStringList{"untouched.png"});QCOMPARE(read(sentinel),original);
    theme->setMode(ThemeMode::Light);QCOMPARE(dialog.previewImage(),light);QCOMPARE(publicSnapshotInputs(dialog.snapshot()),facts);QCOMPARE(preview->accessibleDescription(),captions);QCOMPARE(status->text(),statusText);
}
void ChartShareDialogTest::themeChangePreservesSaveStatusAndExistingFiles_data(){
    QTest::addColumn<bool>("success");QTest::newRow("saved-status")<<true;QTest::newRow("failed-status")<<false;
}
void ChartShareDialogTest::themeChangePreservesSaveStatusAndExistingFiles(){
    QFETCH(bool,success);auto* theme=KeefiTheme::instance();const auto oldMode=theme->mode();const auto reset=qScopeGuard([=]{theme->setMode(oldMode);});theme->setMode(ThemeMode::Light);
    QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath(success?"saved.png":"missing/failed.png");
    ChartShareDialog dialog(syntheticSnapshot());int choices=0;dialog.setPathChooser([&](QWidget*){++choices;return path;});QSignalSpy saved(&dialog,&ChartShareDialog::saved),failed(&dialog,&ChartShareDialog::saveFailed);dialog.show();
    auto* save=dialog.findChild<QPushButton*>("chartShareSave");auto* status=dialog.findChild<QLabel*>("chartShareStatus");auto* preview=dialog.findChild<QLabel*>("chartSharePreview");QVERIFY(save&&status&&preview);save->click();
    QCOMPARE(choices,1);QCOMPARE(saved.size(),success?1:0);QCOMPARE(failed.size(),success?0:1);
    QVERIFY(status->text().contains(success?QStringLiteral("PNG 저장 완료"):QStringLiteral("PNG를 저장하지 못했습니다")));
    const auto statusText=status->text(),captions=preview->accessibleDescription();const auto facts=publicSnapshotInputs(dialog.snapshot());const auto files=QDir(directory.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot);const auto bytes=read(path);
    theme->setMode(ThemeMode::Dark);
    QCOMPARE(dialog.previewImage().pixelColor(0,0),KeefiTheme::colorsFor(true).bg);QCOMPARE(status->text(),statusText);QCOMPARE(preview->accessibleDescription(),captions);QCOMPARE(publicSnapshotInputs(dialog.snapshot()),facts);
    QCOMPARE(choices,1);QCOMPARE(saved.size(),success?1:0);QCOMPARE(failed.size(),success?0:1);QCOMPARE(read(path),bytes);QCOMPARE(QDir(directory.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot),files);
    QVERIFY(save->isEnabled());QVERIFY(dialog.options().maskPersonalValues);QVERIFY(!dialog.options().includePublicDrawings);
}
void ChartShareDialogTest::themeChangeInsideChooserDoesNotTriggerActions(){
    auto* theme=KeefiTheme::instance();const auto oldMode=theme->mode();const auto reset=qScopeGuard([=]{theme->setMode(oldMode);});theme->setMode(ThemeMode::Light);
    QTemporaryDir directory;QVERIFY(directory.isValid());auto input=syntheticSnapshot();input.drawings={{ChartDrawingKind::Horizontal,{{input.closed.first().startsAtMs,"100.5"}}}};
    ChartShareDialog dialog(input);dialog.show();auto* save=dialog.findChild<QPushButton*>("chartShareSave");auto* drawings=dialog.findChild<QCheckBox*>("chartShareDrawings");auto* status=dialog.findChild<QLabel*>("chartShareStatus");auto* preview=dialog.findChild<QLabel*>("chartSharePreview");QVERIFY(save&&drawings&&status&&preview);
    const auto facts=publicSnapshotInputs(dialog.snapshot());const auto statusText=status->text(),captions=preview->accessibleDescription();QSignalSpy saved(&dialog,&ChartShareDialog::saved),failed(&dialog,&ChartShareDialog::saveFailed);
    int choices=0;bool disabledBefore=false,disabledAfter=false,inkChanged=false,optionsStayed=false;
    dialog.setPathChooser([&](QWidget*){++choices;disabledBefore=!save->isEnabled()&&!drawings->isEnabled();theme->setMode(ThemeMode::Dark);
        disabledAfter=!save->isEnabled()&&!drawings->isEnabled();inkChanged=dialog.previewImage().pixelColor(0,0)==KeefiTheme::colorsFor(true).bg;
        drawings->setChecked(true);optionsStayed=!drawings->isChecked()&&!dialog.options().includePublicDrawings;return QString{};});
    save->click();QCOMPARE(choices,1);QVERIFY(disabledBefore);QVERIFY(disabledAfter);QVERIFY(inkChanged);QVERIFY(optionsStayed);
    QCOMPARE(status->text(),statusText);QCOMPARE(preview->accessibleDescription(),captions);QCOMPARE(publicSnapshotInputs(dialog.snapshot()),facts);QVERIFY(dialog.options().maskPersonalValues);QCOMPARE(dialog.options().outputSize,QSize(1200,900));
    QVERIFY(save->isEnabled());QVERIFY(drawings->isEnabled());QCOMPARE(saved.size(),0);QCOMPARE(failed.size(),0);QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
}
void ChartShareDialogTest::shareControlsHave44PixelTargets_data(){
    QTest::addColumn<bool>("dark");QTest::addColumn<QString>("controlName");
    for(const bool dark:{false,true})for(const auto* name:{"chartShareSave","chartShareClose","chartShareDrawings"})
        QTest::newRow(qPrintable(QString(dark?"dark-":"light-")+name))<<dark<<QString(name);
}
void ChartShareDialogTest::shareControlsHave44PixelTargets(){
    QFETCH(bool,dark);QFETCH(QString,controlName);auto* theme=KeefiTheme::instance();const auto previous=theme->mode();
    const auto restore=qScopeGuard([=]{theme->setMode(previous);});theme->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    ChartShareDialog dialog(syntheticSnapshot());dialog.setStyleSheet(KeefiTheme::localStyleSheet(&dialog));dialog.show();
    auto* control=dialog.findChild<QWidget*>(controlName);QVERIFY(control);QVERIFY(control->isVisible());
    QVERIFY2(control->height()>=44,qPrintable(controlName+" height "+QString::number(control->height())));
    QVERIFY(control->width()>=44);
}
void ChartShareDialogTest::publicDrawingsToggleFromTheRowEdgeWithoutSaving(){
    auto input=syntheticSnapshot();input.drawings={{ChartDrawingKind::Horizontal,{{input.closed.first().startsAtMs,"100.5"}}}};
    ChartShareDialog dialog(input);dialog.setStyleSheet(KeefiTheme::localStyleSheet(&dialog));int choices=0;
    dialog.setPathChooser([&](QWidget*){++choices;return QString{};});dialog.show();
    auto* drawings=dialog.findChild<QCheckBox*>("chartShareDrawings");QVERIFY(drawings&&drawings->isEnabled());
    const auto facts=publicSnapshotInputs(dialog.snapshot());const auto before=dialog.previewImage();
    QTest::mouseClick(drawings,Qt::LeftButton,Qt::NoModifier,QPoint(drawings->width()-2,drawings->height()/2));
    QVERIFY(drawings->isChecked());QVERIFY(dialog.options().includePublicDrawings);QVERIFY(dialog.previewImage()!=before);
    QTest::mouseClick(drawings,Qt::LeftButton,Qt::NoModifier,QPoint(drawings->width()-2,drawings->height()/2));
    QVERIFY(!drawings->isChecked());QVERIFY(!dialog.options().includePublicDrawings);QCOMPARE(dialog.previewImage(),before);
    QCOMPARE(publicSnapshotInputs(dialog.snapshot()),facts);QVERIFY(dialog.options().maskPersonalValues);QCOMPARE(choices,0);
}
QTEST_MAIN(ChartShareDialogTest)
#include "tst_chart_share_dialog.moc"
