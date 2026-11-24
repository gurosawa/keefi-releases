#include "ChartInstrumentPanelTestSupport.h"
#include "keefi/SegmentedControlStyle.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/LocalWindow.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/MarketCandles.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QScrollArea>
#include <QScrollBar>
#include "keefi/Vault.h"
#include "keefi/AccountModel.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTransport.h"
#include <QComboBox>
#include <QAbstractItemView>
#include <QBoxLayout>
#include <QTableWidget>
#include <QDialog>
#include <QDir>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStyle>
#include <QStyleHints>
#include <QStyleOptionComboBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTimeZone>
using namespace keefi;

// In-process synthetic replies only: this object has no network implementation.
class SyntheticThemeTransport final : public TradingTransport {
  public:
    int requests=0;
    void send(quint64 token,const RequestSpec& request) override {
        ++requests; QVERIFY(!request.authenticated); QVERIFY(request.headers.isEmpty());
        HttpResult result; result.status=200; QJsonArray rows;
        if(request.operation==Operation::Markets) {
            rows={QJsonObject{{"market","KRW-SYNUP"},{"korean_name","합성 상승"},{"english_name","Synthetic Up"}},
                  QJsonObject{{"market","KRW-SYNDOWN"},{"korean_name","합성 하락"},{"english_name","Synthetic Down"}}};
        } else {
            QCOMPARE(request.operation,Operation::Quote);
            const auto timestamp=QString::number(QDateTime::currentMSecsSinceEpoch()-100);
            rows={QJsonObject{{"market","KRW-SYNUP"},{"trade_price","12345"},{"prev_closing_price","12000"},
                             {"signed_change_rate","0.02875"},{"timestamp",timestamp},{"acc_trade_price_24h","1000000"}},
                  QJsonObject{{"market","KRW-SYNDOWN"},{"trade_price","8765"},{"prev_closing_price","9000"},
                             {"signed_change_rate","-0.02611"},{"timestamp",timestamp},{"acc_trade_price_24h","900000"}}};
        }
        result.body=QJsonDocument(rows).toJson(QJsonDocument::Compact);
        QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});
    }
    void abort() override {}
};

class ThemeTest final : public QObject {
    Q_OBJECT
    QTemporaryDir temporary_;
  private slots:
    void compactSegmentUsesSpecifiedSizeAndPressedFill() {
        QPushButton button(QStringLiteral("On"));
        applySegmentedControlStyle(&button, 28);
        QCOMPARE(button.minimumHeight(), 28);
        QCOMPARE(button.maximumHeight(), 28);
        button.resize(button.sizeHint().width(), 28);
        button.show();
        QTest::mousePress(&button, Qt::LeftButton);
        const auto image = button.grab().toImage();
        const auto color = image.pixelColor(qRound(image.devicePixelRatio() * 4), image.height() / 2);
        QTest::mouseRelease(&button, Qt::LeftButton);
        QCOMPARE(color, themeColors().rule);
    }

    void initTestCase() {
        QVERIFY(temporary_.isValid());
        QCoreApplication::setOrganizationName("KEEFI Synthetic D1");
        QCoreApplication::setApplicationName("Synthetic Theme");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary_.path());
        QVERIFY(KeefiFonts::registerBundledFonts());
    }
    void cleanup() { KeefiTheme::instance()->setSettings(nullptr); }
    void comboUsesFlatInputPaint_data(){
        QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
    }
    void comboUsesFlatInputPaint(){
        QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        QComboBox combo;combo.addItem(QStringLiteral("합성 선택"),1);combo.addItem(QStringLiteral("합성 두 번째"),2);combo.setFocusPolicy(Qt::NoFocus);
        combo.setStyleSheet(KeefiTheme::localStyleSheet(&combo));combo.resize(240,40);combo.show();QCoreApplication::processEvents();
        const auto pixmap=combo.grab();const auto image=pixmap.toImage();const auto dpr=pixmap.devicePixelRatio();
        const auto at=[&](int x,int y){return image.pixelColor(qRound(x*dpr),qRound(y*dpr));};
        for(int y=5;y<combo.height()-5;++y)QCOMPARE(at(140,y),themeColors().input);
        QCOMPARE(at(combo.width()/2,0),themeColors().field);
        QStyleOptionComboBox option;option.initFrom(&combo);option.currentText=combo.currentText();
        const auto arrow=combo.style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxArrow,&combo);
        QVERIFY(!arrow.isEmpty());QCOMPARE(at(arrow.center().x(),8),themeColors().input);QCOMPARE(at(arrow.center().x(),combo.height()-9),themeColors().input);
        int arrowInk=0;for(int y=arrow.center().y()-5;y<=arrow.center().y()+5;++y)for(int x=arrow.center().x()-5;x<=arrow.center().x()+5;++x)
            arrowInk+=at(x,y)!=themeColors().input;
        QVERIFY2(arrowInk>0,"The native dropdown arrow must remain visible above the flat input background");
        QCOMPARE(combo.currentText(),QStringLiteral("합성 선택"));
        const auto out=qEnvironmentVariable("KEEFI_FINAL_F08_RENDER_DIR");if(!out.isEmpty()){
            QVERIFY(QDir().mkpath(out));QVERIFY(combo.grab().save(out+"/combo-"+(dark?"dark":"light")+".png"));
        }
        combo.setFocusPolicy(Qt::StrongFocus);combo.setFocus();QTest::keyClick(&combo,Qt::Key_Down);
        QCOMPARE(combo.currentIndex(),1);QCOMPARE(combo.currentData().toInt(),2);QCOMPARE(combo.height(),40);
        combo.showPopup();QTRY_VERIFY(combo.view()->isVisible());QTest::keyClick(combo.view(),Qt::Key_Up);QTest::keyClick(combo.view(),Qt::Key_Return);
        QTRY_VERIFY(!combo.view()->isVisible());QCOMPARE(combo.currentIndex(),0);QCOMPARE(combo.height(),40);
    }
    void inputUsesFieldBorderWithoutChangingNativeEditing_data(){
        QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
    }
    void inputUsesFieldBorderWithoutChangingNativeEditing(){
        QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        const auto original=QStringLiteral("합성 입력 123.45");QLineEdit input(original);input.setFocusPolicy(Qt::NoFocus);
        input.setStyleSheet(KeefiTheme::localStyleSheet(&input));input.resize(280,40);input.show();QCoreApplication::processEvents();
        QVERIFY(!input.hasFocus());const auto topBorder=[&]{const auto pixmap=input.grab();return pixmap.toImage().pixelColor(qRound(input.width()/2*pixmap.devicePixelRatio()),0);};
        QCOMPARE(topBorder(),themeColors().field);QCOMPARE(input.text(),original);
        const auto out=qEnvironmentVariable("KEEFI_FINAL_F09_RENDER_DIR");if(!out.isEmpty()){
            QVERIFY(QDir().mkpath(out));QVERIFY(input.grab().save(out+"/input-"+(dark?"dark":"light")+".png"));
        }
        input.setSelection(3,2);QCOMPARE(input.selectedText(),original.mid(3,2));
        input.setFocusPolicy(Qt::StrongFocus);input.activateWindow();input.setFocus();QTRY_VERIFY(input.hasFocus());QCoreApplication::processEvents();
        QCOMPARE(topBorder(),themeColors().accent);QCOMPARE(input.text(),original);
        QSignalSpy edited(&input,&QLineEdit::textEdited);QTest::keyClick(&input,Qt::Key_End);QTest::keyClick(&input,Qt::Key_Left,Qt::ShiftModifier);
        QCOMPARE(input.selectedText(),original.right(1));QCOMPARE(input.cursorPosition(),original.size()-1);QCOMPARE(input.text(),original);QCOMPARE(edited.count(),0);
    }
    void readableInkOnBothApprovedBackgrounds() {
        for (const bool dark : {false,true}) {
            const auto& c=KeefiTheme::colorsFor(dark);
            for (const auto& ink : {c.text,c.text2,c.text3,c.accentText,c.upText,c.downText,c.koreanDownText,c.danger,
                                   c.studyPurple,c.studyBlue,c.studyOrange,c.sessionDay}) {
                for (const auto& background : {c.bg,c.surface}) {
                    const auto ratio=KeefiTheme::contrast(ink,background);
                    QVERIFY2(ratio>=4.5,qPrintable(QString("%1 on %2 = %3").arg(ink.name(),background.name()).arg(ratio)));
                }
            }
        }
    }
    void fillsKeepTheApprovedMockupValues() {
        const auto& c=KeefiTheme::colorsFor(false);
        QCOMPARE(c.bg,QColor("#FFFFFF")); QCOMPARE(c.surface,QColor("#F2F4F6"));
        QCOMPARE(c.accent,QColor("#D9480F")); QCOMPARE(c.up,QColor("#0E8A45")); QCOMPARE(c.down,QColor("#D92332"));
        QVERIFY(c.accent!=c.accentText); QVERIFY(c.up!=c.upText); QVERIFY(c.down!=c.downText);
        const auto& d=KeefiTheme::colorsFor(true);
        QCOMPARE(d.bg,QColor("#17171C")); QCOMPARE(d.surface,QColor("#22222A"));
        QCOMPARE(d.accent,QColor("#FF7A45")); QCOMPARE(d.up,QColor("#3BD37F")); QCOMPARE(d.down,QColor("#FF6066"));
    }
    void profileStoresOnlyTheChosenPublicMode() {
        QSettings first(temporary_.filePath("theme.ini"),QSettings::IniFormat);
        auto* theme=KeefiTheme::instance(); theme->setSettings(&first);
        QCOMPARE(theme->mode(),ThemeMode::System);
        QCOMPARE(theme->isDark(),QGuiApplication::styleHints()->colorScheme()==Qt::ColorScheme::Dark);
        theme->setMode(ThemeMode::Dark); first.sync();
        QCOMPARE(first.allKeys(),QStringList{"display/theme/v1"}); QCOMPARE(first.value("display/theme/v1").toString(),QString("dark"));
        QSettings reloaded(temporary_.filePath("theme.ini"),QSettings::IniFormat); theme->setSettings(&reloaded);
        QCOMPARE(theme->mode(),ThemeMode::Dark); QVERIFY(theme->isDark());
        theme->setMode(ThemeMode::Light); QCOMPARE(reloaded.value("display/theme/v1").toString(),QString("light"));
        reloaded.setValue("display/theme/v1","unrecognized"); theme->setSettings(&reloaded);
        QCOMPARE(theme->mode(),ThemeMode::System);
    }
    void openWindowsAndDialogRefreshWithoutChangingMovementState() {
        Vault vault(temporary_.filePath("synthetic-absent.keefi-vault")); AccountModel accounts(&vault);
        PortfolioController portfolio(vault); portfolio.setAutomaticRefresh(false);
        LocalWindow first(vault,accounts),second(vault,accounts);
        first.setPortfolioController(&portfolio); second.setPortfolioController(&portfolio);
        QSettings profile(temporary_.filePath("windows.ini"),QSettings::IniFormat);
        first.setChartSettings(&profile); second.setChartSettings(&profile);
        auto* theme=KeefiTheme::instance(); theme->setMode(ThemeMode::Light);
        auto* policy=first.findChild<MovementColorPolicy*>(MovementColorPolicy::ObjectName); QVERIFY(policy);
        const auto state=policy->publicState(); QSignalSpy changed(policy,&MovementColorPolicy::changed);
        auto* options=first.findChild<QPushButton*>("openOptions"); QVERIFY(options); options->click();
        auto* dialog=first.findChild<QDialog*>("optionsDialog");
        if(!dialog) for(auto* candidate:first.findChildren<QDialog*>()) if(candidate->findChild<QWidget*>("optionsNav")){dialog=candidate;break;}
        QVERIFY(dialog);
        auto* darkChoice=first.findChild<QPushButton*>("themeDark"); QVERIFY(darkChoice); darkChoice->click();
        QVERIFY(theme->isDark()); QVERIFY(changed.count()>0); QCOMPARE(policy->publicState(),state);
        for(auto* window:{&first,&second}) {
            QVERIFY(window->styleSheet().contains(KeefiTheme::colorsFor(true).bg.name()));
            QVERIFY(!window->styleSheet().contains("${")); QVERIFY(window->findChild<QPushButton*>("themeDark")->isChecked());
        }
        QCOMPARE(dialog->styleSheet(),first.styleSheet()); QCOMPARE(profile.value("display/theme/v1").toString(),QString("dark"));
        darkChoice->click(); QVERIFY(darkChoice->isChecked());
        dialog->close();
    }
    void semanticTemplatesRefreshExistingWidgets() {
        QWidget widget;
        setThemeStyle(&widget,"QWidget { color: ${text3}; background: ${surface}; font-size: ${Body15Px}px; }");
        KeefiTheme::instance()->setMode(ThemeMode::Dark);
        QCOMPARE(widget.styleSheet(),QString("QWidget { color: %1; background: %2; font-size: %3px; }")
            .arg(themeColors().text3.name(),themeColors().surface.name()).arg(KeefiFonts::font(FontRole::Body15).pixelSize()));
        KeefiTheme::instance()->setMode(ThemeMode::Light);
        QVERIFY(widget.styleSheet().contains(KeefiTheme::colorsFor(false).text3.name()));
    }
    void movementFillTextAndRiskRemainSeparate() {
        MovementColorPolicy policy;
        for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}) {
            KeefiTheme::instance()->setMode(mode); const auto& c=themeColors(); const auto danger=c.danger;
            policy.setScheme(MovementScheme::GreenRed);
            QCOMPARE(policy.foreground(MovementDirection::Up),c.upText); QCOMPARE(policy.fill(MovementDirection::Up),c.up);
            policy.setScheme(MovementScheme::KoreanRedBlue);
            QCOMPARE(policy.foreground(MovementDirection::Up),c.downText); QCOMPARE(policy.fill(MovementDirection::Up),c.down);
            QCOMPARE(policy.foreground(MovementDirection::Down),c.koreanDownText); QCOMPARE(themeColors().danger,danger);
        }
    }
    void optionalSyntheticChartShellRenders() {
        const auto output=qEnvironmentVariable("KEEFI_D3_RENDER_DIR");
        if(output.isEmpty()) QSKIP("Optional synthetic D3 shell PNG artifacts; set KEEFI_D3_RENDER_DIR to export.");
        QVERIFY(QDir().mkpath(output));
        class CaptureTransport final : public TradingTransport {
          public:
            quint64 token=0;
            void send(quint64 id,const RequestSpec& request) override { QVERIFY(!request.authenticated); QVERIFY(!isMutation(request.operation)); token=id; }
            void abort() override {}
        } transport, metadata;
        constexpr qint64 now=1780000020000;
        MarketCandles candles(&transport,std::unique_ptr<PublicCandleSocket>{},[]{return now;});
        Vault vault(temporary_.filePath("d3-shell-absent.keefi-vault")); AccountModel accounts(&vault);
        PortfolioController portfolio(vault);portfolio.setAutomaticRefresh(false);
        QSettings profile(temporary_.filePath("d3-shell.ini"),QSettings::IniFormat);
        LocalWindow window(vault,accounts);window.setPortfolioController(&portfolio);window.setChartSettings(&profile);
        window.setChartInstrumentTransport(&metadata);window.setMarketCandles(&candles);window.setSecurityStatus(true);
        auto* workspace=window.findChild<ChartWorkspace*>("chartWorkspace");QVERIFY(workspace);
        workspace->indicatorPreferences()->markIntroSeen();workspace->setIdentity("coin:upbit:KRW-SYN");workspace->setTimeframe(60000);
        auto* pages=window.findChild<QTabWidget*>("portfolioPages");QVERIFY(pages);pages->setCurrentWidget(workspace);
        window.resize(1440,1320);window.show();QTRY_VERIFY(transport.token);
        QJsonArray rows;const int pattern[]{100,101,99,102,100,104,101,103,100,105,102,106,103,107,104,108,106,109,107,110};
        for(int i=0;i<200;++i){const auto at=now/60000*60000-(200-i)*60000;const auto price=pattern[i%20];
            rows.append(QJsonObject{{"market","KRW-SYN"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,QTimeZone::UTC).toString("yyyy-MM-ddTHH:mm:ss")},
                {"timestamp",QString::number(at+1000)},{"unit","1"},{"opening_price",QString::number(price+(i%2?1:-1))},
                {"high_price",QString::number(i==191?180:price+2)},{"low_price",QString::number(i==190?90:price-2)},
                {"trade_price",QString::number(price)},{"candle_acc_trade_volume",QString::number(2+i%10)}});
        }
        HttpResult result;result.status=200;result.body=QJsonDocument(rows).toJson();emit transport.finished(transport.token,result);
        QVERIFY(candles.series().available);
        auto* lists=workspace->watchlistModel();const WatchlistIdentity selected{"coin","upbit","KRW-SYN"},second{"coin","upbit","KRW-DEMO"};
        lists->setVerifiedInstruments({selected,second});const auto list=lists->createList("합성 관심 리스트");lists->addIdentity(list,selected);lists->addIdentity(list,second);lists->useList(list);
        lists->setRowState(selected,{"110 KRW","+2.87%","합성 공개 관측",{"눌림존 7/10"},now-60000,false,{{"눌림존 7/10",MovementDirection::Unknown,false}}});
        lists->setRowState(second,{"98 KRW","-1.23%","합성 공개 관측",{"관측 대기"},now-60000,false,{}});
        for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}) {
            KeefiTheme::instance()->setMode(mode);
            for(const auto size:{QSize(1440,1320),QSize(1280,1000),QSize(720,1000)}) {
                window.resize(size);QCoreApplication::processEvents();
                auto* scroll=workspace->findChild<QScrollArea*>("chartWorkspaceScroll");QVERIFY(scroll);scroll->verticalScrollBar()->setValue(0);
                QCoreApplication::processEvents();
                const auto stem=QString("d3-shell-%1-%2").arg(mode==ThemeMode::Light?"light":"dark").arg(size.width());
                QVERIFY(window.grab().save(QDir(output).filePath(stem+".png")));
                auto* panel=test::openInstrumentPanel(*workspace);QVERIFY(panel);
                auto* watchlist=panel->findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(watchlist);
                for(QTreeWidgetItemIterator it(watchlist);*it;++it) if((*it)->data(0,Qt::UserRole).toString()=="coin:upbit:KRW-SYN") {watchlist->setCurrentItem(*it);break;}
                if(size.width()==720)QVERIFY(panel->grab().save(QDir(output).filePath(stem+"-lower.png")));
                panel->close();
            }
        }
    }
    void optionalSyntheticWindowRenders() {
        const auto output=qEnvironmentVariable("KEEFI_D1_RENDER_DIR");
        if(output.isEmpty()) QSKIP("Optional synthetic D1 PNG artifacts; set KEEFI_D1_RENDER_DIR to export.");
        QVERIFY(QDir().mkpath(output));
        Vault vault(temporary_.filePath("render-absent.keefi-vault")); AccountModel accounts(&vault);
        PortfolioController portfolio(vault); portfolio.setAutomaticRefresh(false);
        SyntheticThemeTransport transport; PublicMarketCatalog catalog(&transport);
        catalog.refresh(); QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(),1000);
        QCOMPARE(catalog.rows().size(),2); QCOMPARE(transport.requests,2);
        LocalWindow window(vault,accounts); window.setPortfolioController(&portfolio); window.setPublicMarketCatalog(&catalog,false);
        window.setSecurityStatus(true); window.resize(1280,940); window.show();
        auto* view=window.findChild<QComboBox*>("exploreView"); QVERIFY(view); view->setCurrentIndex(view->findData("list"));
        auto* section=window.findChild<QComboBox*>("exploreMarketSection"); QVERIFY(section);
        auto* filters=window.findChild<QBoxLayout*>("exploreFilters"); QVERIFY(filters);
        auto* search=window.findChild<QLineEdit*>("exploreSearch"); QVERIFY(search); search->setText("SYN");
        auto* rows=window.findChild<QTableWidget*>("exploreMarkets"); QVERIFY(rows); QCOMPARE(rows->rowCount(),2); rows->setCurrentCell(0,0);
        QSettings profile(temporary_.filePath("render.ini"),QSettings::IniFormat); window.setChartSettings(&profile);
        for (const auto mode:{ThemeMode::Light,ThemeMode::Dark}) {
            KeefiTheme::instance()->setMode(mode); QCoreApplication::processEvents();
            const auto name=mode==ThemeMode::Light?"d1-light":"d1-dark";
            for(const int width:{1280,820}) {
                window.resize(width,940);
                // resizeEvent queues the existing adaptive filter reflow. Wait
                // for the required geometry, not an arbitrary sleep duration.
                const auto direction=width<1050?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight;
                QTRY_COMPARE_WITH_TIMEOUT(filters->direction(),direction,3000);
                const auto settled=[&] {
                    const QRect a(view->mapTo(&window,QPoint{}),view->size());
                    const QRect b(section->mapTo(&window,QPoint{}),section->size());
                    const QRect c(search->mapTo(&window,QPoint{}),search->size());
                    if(a.intersects(b)||b.intersects(c)||a.intersects(c))return false;
                    if(width<1050)return a.bottom()<b.top()&&b.bottom()<c.top();
                    return a.right()<b.left()&&b.right()<c.left()&&
                        qAbs(a.center().y()-b.center().y())<=1&&qAbs(b.center().y()-c.center().y())<=1;
                };
                QTRY_VERIFY_WITH_TIMEOUT(settled(),3000);
                QVERIFY(window.grab().save(QDir(output).filePath(QString("%1-%2.png").arg(name).arg(width)),"PNG"));
            }
        }
        QCOMPARE(transport.requests,2);
    }
};
QTEST_MAIN(ThemeTest)
#include "tst_theme.moc"
