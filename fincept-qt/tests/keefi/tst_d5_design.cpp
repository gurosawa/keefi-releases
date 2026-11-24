#include "keefi/AccountModel.h"
#include "keefi/BackgroundTray.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/LocalTableWidget.h"
#include "keefi/SegmentedComboBox.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QAction>
#include <QDialog>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTabWidget>
#include <QScrollArea>
#include <QTimer>
#include <array>
using namespace keefi;

class D5Tray final : public BackgroundTray {
 public:
  QMenu* menu=nullptr;bool shown=false;
  bool available()const override{return true;}
  bool visible()const override{return shown;}
  void setMenu(QMenu* value)override{menu=value;}
  void show()override{shown=true;}
  void hide()override{shown=false;}
};
class D5ReadTransport final:public TradingTransport {
 public:
  bool holdResponse = false;
  quint64 pendingToken = 0;

  void abort()override{}
  void send(quint64 token,const RequestSpec& request)override{
   QCOMPARE(request.operation,Operation::Account);QVERIFY(!isMutation(request.operation));
   if (holdResponse) {
    pendingToken = token;
    return;
   }
   complete(token);
  }
  void complete(quint64 token, bool fail = false) {
   HttpResult result;result.status=fail ? 400 : 200;result.body=R"([{"currency":"KRW","balance":"250000","locked":"0","avg_buy_price":"0","unit_currency":"KRW"}])";
   QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});
  }
};

class D5DesignTest final : public QObject {
 Q_OBJECT
 QTemporaryDir temporary_;
 private slots:
 void initTestCase(){
  QVERIFY(temporary_.isValid());QCoreApplication::setOrganizationName("KEEFI Synthetic D5");
  QCoreApplication::setApplicationName("D5 Synthetic Design");QCoreApplication::setApplicationVersion("0.18.0-g8-preview-dev");
  QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary_.filePath("settings"));
  QVERIFY(KeefiFonts::registerBundledFonts());
 }
 void cleanup(){KeefiTheme::instance()->setSettings(nullptr);}
 void nativeEditorsUseTheInputToken(){
  for(auto mode:{ThemeMode::Light,ThemeMode::Dark}){
   KeefiTheme::instance()->setMode(mode);QPlainTextEdit editor;QDoubleSpinBox number;
   QCOMPARE(editor.palette().color(QPalette::Base),themeColors().input);
   QCOMPARE(number.palette().color(QPalette::Base),themeColors().input);
   QCOMPARE(editor.palette().color(QPalette::Window),themeColors().bg);
  }
 }
 void wrappedTableCellsKeepNativeLayout(){
  LocalTableWidget table(1,1);table.setFont(KeefiFonts::font(FontRole::Body15));
  table.setItem(0,0,new QTableWidgetItem("보존해야 하는\n긴 한글 본문"));
  QStyleOptionViewItem option;option.initFrom(&table);option.widget=&table;option.rect=QRect(0,0,100,100);option.features|=QStyleOptionViewItem::WrapText;
  QImage actual(100,100,QImage::Format_ARGB32_Premultiplied),expected=actual;actual.fill(Qt::transparent);expected.fill(Qt::transparent);
  QPainter first(&actual);table.itemDelegate()->paint(&first,option,table.model()->index(0,0));first.end();
  QStyledItemDelegate native;QPainter second(&expected);native.paint(&second,option,table.model()->index(0,0));second.end();
  QCOMPARE(actual,expected);QCOMPARE(table.item(0,0)->text(),QString("보존해야 하는\n긴 한글 본문"));
 }
 void connectionTypographyKeepsNativeSelectionAndLockClearing(){
  QTemporaryDir temp;QVERIFY(temp.isValid());Vault vault(temp.filePath("synthetic-d5-vault"));QVERIFY(vault.create("Synthetic D5 Password! 2026"));
  AccountRecord record;record.provider="upbit";record.environment="live";record.market="spot";record.currency="KRW";record.alias="합성 D5 연결";
  QVERIFY(vault.addAccount(record,{{"access_key","FAKE_D5_KEY"},{"secret","FAKE_D5_SECRET"}},StorageMode::EncryptedVault));
  AccountModel accounts(&vault);D5ReadTransport http;PortfolioController::Factories factories;
  factories.trading=[&](const AccountRecord&){return &http;};factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
  D5Tray tray;LocalWindow window(vault,accounts,nullptr,&tray);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.show();portfolio.refreshAll();
  auto* time=window.findChild<QLabel*>("connectionReadTime");QVERIFY(time);QTRY_VERIFY_WITH_TIMEOUT(time->text().contains("최근 성공"),5000);
  QCOMPARE(time->font().family(),KeefiFonts::bodyFamily());QCOMPARE(time->font().pixelSize(),12);
  auto* pages=window.findChild<QTabWidget*>("portfolioPages");auto* connections=window.findChild<QScrollArea*>("connectionsPage");QVERIFY(pages);QVERIFY(connections);pages->setCurrentWidget(connections);
  QVERIFY2(connections->findChild<QWidget*>("avatarBadge")==nullptr,"Connections use service names without an initial avatar");
  auto* more=window.findChild<QPushButton*>("accountMoreToggle");QVERIFY(more);more->setChecked(true);
  auto* detail=window.findChild<QLabel*>("accountDetails");QVERIFY(detail);QTextDocument doc;doc.setHtml(detail->text());const auto plain=doc.toPlainText();QVERIFY(!plain.isEmpty());
  detail->setSelection(0,plain.size());QCOMPARE(QString(detail->selectedText()).replace(QChar::LineSeparator,'\n'),plain);
  detail->setSelection(0,0);
  QTest::qWait(20);
  for(auto* control:connections->findChildren<QPushButton*>())if(control->isVisibleTo(connections))QVERIFY2(control->height()>=28,qPrintable(control->objectName()));
  for(auto* control:connections->findChildren<SegmentedComboBox*>())if(control->isVisibleTo(connections))for(const auto& rect:control->segmentRects())if(!rect.isEmpty())QVERIFY(rect.height()>=28);
  const auto out=qEnvironmentVariable("KEEFI_D5_RENDER_DIR");if(!out.isEmpty()){
   QDir().mkpath(out);for(auto mode:{ThemeMode::Light,ThemeMode::Dark}){KeefiTheme::instance()->setMode(mode);for(int width:{1440,720}){
    window.resize(width,1050);QTest::qWait(40);const auto prefix=out+"/connections-"+(mode==ThemeMode::Light?"light":"dark")+"-"+QString::number(width);
    for(auto* control:connections->findChildren<QPushButton*>())if(control->isVisibleTo(connections))QVERIFY2(control->height()>=28,qPrintable(control->objectName()));
    for(auto* control:connections->findChildren<SegmentedComboBox*>())if(control->isVisibleTo(connections))for(const auto& rect:control->segmentRects())if(!rect.isEmpty())QVERIFY(rect.height()>=28);
    QVERIFY(window.grab().save(prefix+".png"));if(width==720){auto* list=window.findChild<QListView*>("accountList");QVERIFY(list);const auto index=list->model()->index(0,0);
     QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,list->visualRect(index).center());QTest::qWait(30);
     for(auto* control:connections->findChildren<QPushButton*>())if(control->isVisibleTo(connections))QVERIFY2(control->height()>=28,qPrintable(control->objectName()));
     QVERIFY(window.grab().save(prefix+"-detail.png"));window.findChild<QPushButton*>("connectionDetailBack")->click();}
   }}
  }
  window.lockAndClear();QVERIFY(detail->text().isEmpty());QVERIFY(time->text().isEmpty());
 }
 void connectionHeaderKeepsGeometryAndLastSuccessDuringStatusChanges(){
  QTemporaryDir temp;
  QVERIFY(temp.isValid());
  Vault vault(temp.filePath("synthetic-header-vault"));
  QVERIFY(vault.create("Synthetic D5 Password! 2026"));
  AccountRecord record;
  record.provider = "upbit";
  record.environment = "live";
  record.market = "spot";
  record.currency = "KRW";
  record.alias = "합성 연결 계좌";
  QVERIFY(vault.addAccount(record, {{"access_key", "FAKE_D5_KEY"}, {"secret", "FAKE_D5_SECRET"}}, StorageMode::EncryptedVault));

  D5ReadTransport http;
  http.holdResponse = true;
  PortfolioController::Factories factories;
  factories.trading = [&](const AccountRecord&) { return &http; };
  factories.prices = [] { return static_cast<TradingTransport*>(nullptr); };
  PortfolioController portfolio(vault, nullptr, factories);
  AccountModel accounts(&vault);
  D5Tray tray;
  LocalWindow window(vault, accounts, nullptr, &tray);
  window.setPortfolioController(&portfolio);
  window.setSecurityStatus(true);
  window.findChild<QTabWidget*>("portfolioPages")->setCurrentWidget(window.findChild<QWidget*>("connectionsPage"));
  window.resize(1440, 900);
  window.show();

  auto* list = window.findChild<QListView*>("accountList");
  auto* detail = window.findChild<QWidget*>("connectionDetail");
  auto* header = detail ? detail->findChild<QWidget*>("connectionDetailHeader") : nullptr;
  auto* state = window.findChild<QLabel*>("connectionReadState");
  auto* lastSuccess = window.findChild<QLabel*>("connectionReadTime");
  auto* refresh = window.findChild<QPushButton*>("refreshSelectedConnection");
  auto* diagnostics = window.findChild<QPushButton*>("accountMoreToggle");
  auto* holdings = window.findChild<QPushButton*>("connectionAssets");
  auto* title = window.findChild<QLabel*>("accountTitle");
  QVERIFY(list && header && state && lastSuccess && refresh && diagnostics && holdings && title);
  auto* remove = window.findChild<QPushButton*>("removeAccount");
  QVERIFY(remove && remove->isVisible() && remove->isEnabled());
  QCOMPARE(remove->palette().color(QPalette::ButtonText), themeColors().danger);
  const auto buttonImage = remove->grab().toImage();
  QCOMPARE(buttonImage.pixelColor(qRound(5 * buttonImage.devicePixelRatio()), buttonImage.height() / 2),
           themeColors().bg);
  QTRY_VERIFY_WITH_TIMEOUT(http.pendingToken != 0, 5000);
  QTRY_COMPARE(state->text(), QStringLiteral("첫 조회 중"));
  const auto controls = std::array<QWidget*, 6>{title, state, lastSuccess, refresh, diagnostics, holdings};
  const auto geometry = [&] {
   QList<QRect> bounds;
   for (auto* control : controls) bounds.append({control->mapTo(detail, QPoint{}), control->size()});
   return bounds;
  };
  for (auto* control : controls) QVERIFY2(control->isVisible(), qPrintable(control->objectName()));
  QCOMPARE(list->width(), 360);
  QCOMPARE(header->height(), 72);
  QCOMPARE(state->height(), 22);
  QCOMPARE(lastSuccess->height(), 22);
  const auto settled = geometry();
  http.holdResponse = false;
  http.complete(http.pendingToken);
  QTRY_COMPARE_WITH_TIMEOUT(state->text(), QStringLiteral("조회 완료"), 5000);
  QCOMPARE(geometry(), settled);
  QCOMPARE(state->palette().color(QPalette::WindowText), themeColors().text2);
  const auto successText = lastSuccess->text();
  QVERIFY(successText.contains("최근 성공"));
  const auto selection = list->currentIndex();
  list->setFocus();

  http.holdResponse = true;
  http.pendingToken = 0;
  refresh->click();
  QTRY_VERIFY_WITH_TIMEOUT(http.pendingToken != 0, 3000);
  QTRY_VERIFY(!refresh->isEnabled());
  QCOMPARE(geometry(), settled);
  QCOMPARE(lastSuccess->text(), successText);
  QCOMPARE(list->currentIndex(), selection);
  QCOMPARE(window.focusWidget(), list);

  http.holdResponse = false;
  http.complete(http.pendingToken, true);
  QTRY_COMPARE_WITH_TIMEOUT(state->text(), QStringLiteral("조회 실패"), 5000);
  QCOMPARE(state->palette().color(QPalette::WindowText), themeColors().danger);
  QCOMPARE(geometry(), settled);
  QCOMPARE(lastSuccess->text(), successText);

  window.resize(1099, 700);
  QTRY_VERIFY(list->isVisible() && !detail->isVisible());
  QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualRect(selection).center());
  QTRY_VERIFY(!list->isVisible() && detail->isVisible());
  window.findChild<QPushButton*>("connectionDetailBack")->click();
  QTRY_VERIFY(list->isVisible() && !detail->isVisible());
  window.resize(1100, 700);
  QTRY_VERIFY(list->isVisible() && detail->isVisible());
 }
 void settingsAndMenuKeepTheirTextAndLayout(){
  Vault vault(temporary_.filePath("absent-settings-vault"));AccountModel accounts(&vault);D5Tray tray;
  PortfolioController::Factories factories;factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
  PortfolioController portfolio(vault,nullptr,factories);
  LocalWindow window(vault,accounts,nullptr,&tray);window.setPortfolioController(&portfolio);window.show();
  auto* open=window.findChild<QPushButton*>("openOptions");QVERIFY(open);open->click();
  auto* dialog=window.findChild<QDialog*>("optionsDialog");QVERIFY(dialog);
  auto* nav=dialog->findChild<QWidget*>("optionsNav");auto* pages=dialog->findChild<QStackedWidget*>("optionsPages");QVERIFY(nav);QVERIFY(pages);
  // Static navigation must not create Qt 6.8.3 Cocoa table/cell AX objects.
  QVERIFY2(!qobject_cast<QAbstractItemView*>(nav),"Settings navigation must avoid the native item-view AX teardown path");
  const auto choices=nav->findChildren<QPushButton*>(QString(),Qt::FindDirectChildrenOnly);
  QStringList titles;for(auto* choice:choices)titles<<choice->text();
  QCOMPARE(titles,QStringList({"일반","표시","보안","기록","정보","알림","시장 지표 알림"}));QCOMPARE(nav->width(),150);QCOMPARE(pages->count(),7);
  QCOMPARE(choices.size(),7);QVERIFY(choices[0]->isChecked());
  dialog->activateWindow();choices[0]->setFocus();QTest::qWait(20);
  QTest::keyClick(choices[0],Qt::Key_Down);QCOMPARE(pages->currentIndex(),1);QVERIFY(choices[1]->isChecked());QVERIFY(!choices[0]->isChecked());
  QTest::keyClick(choices[1],Qt::Key_Up);QCOMPARE(pages->currentIndex(),0);QVERIFY(choices[0]->isChecked());
  QTest::keyClick(choices[0],Qt::Key_End);QCOMPARE(pages->currentIndex(),6);QVERIFY(choices[6]->isChecked());
  QTest::keyClick(choices[6],Qt::Key_Home);QCOMPARE(pages->currentIndex(),0);QVERIFY(choices[0]->isChecked());
  auto* version=dialog->findChild<QLabel*>("optionVersion");QVERIFY(version);
  QCOMPARE(version->text(),QString("D5 Synthetic Design 0.18.0-g8-preview-dev"));
  QCOMPARE(version->font().family(),KeefiFonts::numberFamily());QCOMPARE(version->font().pixelSize(),11);
  QVERIFY(tray.menu);QStringList actions;for(auto* action:tray.menu->actions())actions<<action->text();
  QCOMPARE(actions,QStringList({"KEEFI 열기","알림 잠시 끄기","드립 끄기","금고 잠금","","앱 종료"}));
  const auto out=qEnvironmentVariable("KEEFI_D5_RENDER_DIR");if(!out.isEmpty())QDir().mkpath(out);
  for(auto mode:{ThemeMode::Light,ThemeMode::Dark}){
   KeefiTheme::instance()->setMode(mode);QTest::qWait(30);QCOMPARE(nav->width(),150);QCOMPARE(dialog->size(),QSize(760,560));
   QCOMPARE(tray.menu->palette().color(QPalette::Window),themeColors().bg);
   for(int page=0;page<choices.size();++page){QTest::mouseClick(choices[page],Qt::LeftButton,Qt::NoModifier,QPoint(choices[page]->width()-2,2));QTest::qWait(20);QCOMPARE(pages->currentIndex(),page);QVERIFY(choices[page]->isChecked());if(!out.isEmpty())QVERIFY(dialog->grab().save(out+"/options-"+(mode==ThemeMode::Light?"light":"dark")+"-"+QString::number(page)+".png"));
    QVERIFY2(choices[page]->height()>=44,"Settings navigation needs a 44 px pointer target");
    for(auto* action:dialog->findChildren<QAbstractButton*>())if(action->isVisibleTo(dialog)&&(qobject_cast<QPushButton*>(action)||qobject_cast<QCheckBox*>(action)))
     QVERIFY2(action->height()>=44&&action->width()>=44,qPrintable(action->objectName()+QString(" %1x%2").arg(action->width()).arg(action->height())));
    for(auto* combo:dialog->findChildren<QComboBox*>())if(combo->isVisibleTo(dialog))QVERIFY2(combo->height()>=44,qPrintable(combo->objectName()));
   }
   choices[0]->click();QTest::qWait(20);
   auto* check=dialog->findChild<QCheckBox*>("optionBackground");QVERIFY(check);const bool initial=check->isChecked();
   QTest::mouseClick(check,Qt::LeftButton,Qt::NoModifier,QPoint(check->width()-2,2));QCOMPARE(check->isChecked(),!initial);
   QTest::mouseClick(check,Qt::LeftButton,Qt::NoModifier,QPoint(check->width()-2,check->height()-2));QCOMPARE(check->isChecked(),initial);
   if(!out.isEmpty()){tray.menu->popup(QPoint(20,20));QTest::qWait(30);QVERIFY(tray.menu->grab().save(out+"/menu-"+(mode==ThemeMode::Light?"light":"dark")+".png"));tray.menu->hide();}
  }
  dialog->resize(560,360);
  for(int page=0;page<choices.size();++page){choices[page]->click();QTest::qWait(20);QCOMPARE(dialog->size(),QSize(560,360));
   auto* scroll=qobject_cast<QScrollArea*>(pages->currentWidget());QVERIFY(scroll);QCOMPARE(scroll->frameShape(),QFrame::NoFrame);
   for(auto* control:pages->currentWidget()->findChildren<QAbstractButton*>())if(control->isVisibleTo(dialog)){
    scroll->ensureWidgetVisible(control);QTest::qWait(1);
    QVERIFY(scroll->viewport()->rect().contains(control->mapTo(scroll->viewport(),control->rect().center())));
   }
  }
  QPointer<QDialog> closing=dialog;QTest::keyClick(choices[6],Qt::Key_Escape);QTRY_VERIFY(closing.isNull());
  open->click();auto* reopened=window.findChild<QDialog*>("optionsDialog");QVERIFY(reopened);QVERIFY(reopened->isVisible());
  QCOMPARE(reopened->findChild<QStackedWidget*>("optionsPages")->currentIndex(),6);reopened->close();
 }
};
QTEST_MAIN(D5DesignTest)
#include "tst_d5_design.moc"
