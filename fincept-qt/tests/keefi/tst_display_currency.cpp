#include "keefi/AccountModel.h"
#include "keefi/CoinbaseQuotes.h"
#include "keefi/InfoButton.h"
#include "keefi/MarketMetrics.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/BackgroundTray.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <QTabWidget>

using namespace keefi;
namespace {
class NoTray final:public BackgroundTray {
  public:
    bool available()const override{return false;}
    bool visible()const override{return false;}
    void setMenu(QMenu*)override{}
    void show()override{}
    void hide()override{}
};
class CashTransport final:public TradingTransport {
  public:
    int reads=0;
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(!isMutation(request.operation));QCOMPARE(request.operation,Operation::Account);++reads;
        HttpResult result;result.status=200;result.body=R"([{"currency":"KRW","balance":"1250000","locked":"0","avg_buy_price":"0","unit_currency":"KRW"}])";
        QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});
    }
    void abort()override{}
};
class PublicFxTransport final:public TradingTransport {
  public:
    int reads=0;quint64 lastToken=0;bool coinbase=false;
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(NetworkTradingTransport::allowed(request));QVERIFY(!request.authenticated);++reads;lastToken=token;
        if(coinbase){QVERIFY(CoinbaseQuotes::isRequest(request));return;}
        HttpResult result;result.status=200;
        if(request.operation==Operation::Markets)result.body=R"([{"market":"KRW-USDT","korean_name":"테더","english_name":"Tether","market_event":{"warning":false,"caution":{}}}])";
        else {QCOMPARE(request.operation,Operation::Quote);result.body=QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-USDT"},{"trade_price","1250"},{"prev_closing_price","1240"},{"signed_change_rate","0.001"},{"timestamp",QString::number(QDateTime::currentMSecsSinceEpoch()-1000)}}}).toJson();}
        QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});
    }
    void abort()override{}
    void replyCoinbase(int status=200){HttpResult result;result.status=status;result.body=QJsonDocument(QJsonObject{{"price","0.998"},{"time",QDateTime::currentDateTimeUtc().addSecs(-1).toString(Qt::ISODateWithMs)}}).toJson();emit finished(lastToken,result);}
};
}
class DisplayCurrencyTest final:public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
  private slots:
    void initTestCase(){
        QVERIFY(settings_.isValid());QCoreApplication::setOrganizationName("KEEFI Synthetic Currency");QCoreApplication::setApplicationName("Synthetic Currency");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.filePath("settings"));
        QVERIFY(KeefiFonts::registerBundledFonts());
    }
    void cleanup(){KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(ThemeMode::System);}
    void choiceDefaultsToKrwAndPersistsOutsideTheVault(){
        QTemporaryDir dir;QVERIFY(dir.isValid());QSettings profile(dir.filePath("synthetic-display.ini"),QSettings::IniFormat);
        Vault vault(dir.filePath("never-created-vault"));AccountModel accounts(&vault);NoTray tray;
        PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        const auto choice=[&](LocalWindow& window){
            auto* button=window.findChild<QPushButton*>("openOptions");if(!button)return static_cast<QComboBox*>(nullptr);
            button->click();auto* displayTab=window.findChild<QPushButton*>("optionsDisplayTab");if(!displayTab)return static_cast<QComboBox*>(nullptr);displayTab->click();
            return window.findChild<QComboBox*>("optionMoneyCurrency");
        };
        {
            LocalWindow window(vault,accounts,nullptr,&tray);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.show();
            auto* currency=choice(window);QVERIFY2(currency,"Options > Display must expose the KRW/USD money display choice");
            QCOMPARE(currency->currentData().toString(),QString("KRW"));QCOMPARE(currency->count(),2);
            currency->setCurrentIndex(currency->findData("USD"));QCOMPARE(profile.value("display/moneyCurrency/v1").toString(),QString("USD"));
            profile.sync();QVERIFY(!vault.exists());QVERIFY(vault.accounts().isEmpty());
        }
        {
            LocalWindow window(vault,accounts,nullptr,&tray);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.show();
            auto* currency=choice(window);QVERIFY(currency);QCOMPARE(currency->currentData().toString(),QString("USD"));
            currency->setCurrentIndex(currency->findData("KRW"));QCOMPARE(profile.value("display/moneyCurrency/v1").toString(),QString("KRW"));
            QVERIFY(!vault.exists());QVERIFY(vault.accounts().isEmpty());
        }
    }
    void existingPublicPairConvertsWithoutStartingReadsAndFailsClosed(){
        QTemporaryDir dir;QVERIFY(dir.isValid());QSettings profile(dir.filePath("synthetic-cross.ini"),QSettings::IniFormat);profile.setValue("display/moneyCurrency/v1","USD");
        Vault vault(dir.filePath("synthetic-cross-vault"));QVERIFY(vault.create("Synthetic Currency Password 2026!"));
        AccountRecord account;account.provider="upbit";account.environment="live";account.market="spot";account.currency="KRW";account.alias="SYNTHETIC_CROSS";
        QVERIFY(vault.addAccount(account,{{"access_key","FAKE_CROSS_ACCESS"},{"secret","FAKE_CROSS_SECRET"}},StorageMode::EncryptedVault));
        AccountModel accounts(&vault);NoTray tray;CashTransport cash;PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &cash;};factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        PublicFxTransport upbit;PublicMarketCatalog catalog(&upbit);catalog.refresh();QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy()&&!catalog.quotes().isEmpty(),3000);catalog.setPresentationActive(false);
        PublicFxTransport cbHttp;cbHttp.coinbase=true;CoinbaseQuotes cb(&cbHttp);cb.setAsset("USDT");cbHttp.replyCoinbase();QCOMPARE(cb.quotes().value("USDT").price,QString("0.998"));
        MarketMetrics metrics(nullptr);LocalWindow window(vault,accounts,nullptr,&tray);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.show();window.setPublicMarketCatalog(&catalog,false);
        auto* total=window.findChild<QLabel*>("portfolioTotal");QVERIFY(total);QTRY_COMPARE_WITH_TIMEOUT(total->text(),QString("1,250,000"),8000);portfolio.setAutomaticRefresh(false);
        const auto snapshots=portfolio.snapshots();const auto cashReads=cash.reads;const auto catalogReads=upbit.reads;const auto cbReads=cbHttp.reads;
        const auto diagnostics=[&]{auto result=window.valuationDiagnostics();result.remove("as_of_ms");result.remove("exported_at_ms");return QJsonDocument(result).toJson(QJsonDocument::Compact);};const auto original=diagnostics();
        window.setMarketMetrics(&metrics,&cb);
        QTRY_COMPARE_WITH_TIMEOUT(total->text(),QString("998.00"),3000);QCOMPARE(window.findChild<QLabel*>("portfolioTotalUnit")->text(),QString("USD"));
        QVERIFY(cb.quotes().isEmpty()); // Existing Flow demand stopped; the passive observation keeps original clocks.
        QCOMPARE(upbit.reads,catalogReads);QCOMPARE(cbHttp.reads,cbReads);QCOMPARE(cash.reads,cashReads);QCOMPARE(portfolio.snapshots(),snapshots);QCOMPARE(diagnostics(),original);
        auto* info=window.findChild<InfoButton*>("portfolioCurrencyInfo");QVERIFY(info);info->click();auto* explanation=info->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(explanation);QVERIFY(explanation->toPlainText().contains("코인베이스"));QVERIFY(explanation->toPlainText().contains("USDT/KRW"));QVERIFY(!explanation->toPlainText().contains("http"));info->findChild<QDialog*>("infoDialog")->close();
        if(const auto out=qEnvironmentVariable("KEEFI_P3_RENDER_DIR");!out.isEmpty()){
            auto* pages=window.findChild<QTabWidget*>("portfolioPages");auto* home=window.findChild<QWidget*>("portfolioHome");QVERIFY(pages&&home);pages->setCurrentWidget(home);
            for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){KeefiTheme::instance()->setMode(mode);const auto theme=mode==ThemeMode::Light?"light":"dark";
                for(int width:{1440,720}){window.resize(width,1050);QTest::qWait(60);QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),3000);QCoreApplication::processEvents();QVERIFY(window.grab().save(out+QString("/assets-usd-cross-%1-%2.png").arg(theme).arg(width)));}}
        }
        QCOMPARE(upbit.reads,catalogReads);QCOMPARE(cbHttp.reads,cbReads);QCOMPARE(cash.reads,cashReads);
        cb.setAsset("USDT");QTRY_COMPARE_WITH_TIMEOUT(cbHttp.reads,cbReads+1,2000);cbHttp.replyCoinbase(503);
        QTRY_COMPARE_WITH_TIMEOUT(total->text(),QString("1,250,000"),3000);QCOMPARE(window.findChild<QLabel*>("portfolioCurrencyStatus")->text(),QStringLiteral("환율 불러오는 중"));QCOMPARE(window.findChild<QLabel*>("portfolioTotalUnit")->text(),QString("KRW"));
        QCOMPARE(portfolio.snapshots(),snapshots);QCOMPARE(diagnostics(),original);QCOMPARE(cash.reads,cashReads);cb.setAsset({});window.lockAndClear();QCOMPARE(total->text(),QString::fromUtf8("—"));QCOMPARE(profile.value("display/moneyCurrency/v1").toString(),QString("USD"));
    }
    void missingRateKeepsKrwAndCannotChangeValuationOrStartReads(){
        QTemporaryDir dir;QVERIFY(dir.isValid());QSettings profile(dir.filePath("synthetic-cash-display.ini"),QSettings::IniFormat);
        Vault vault(dir.filePath("synthetic-cash-vault"));QVERIFY(vault.create("Synthetic Currency Password 2026!"));
        AccountRecord account;account.provider="upbit";account.environment="live";account.market="spot";account.currency="KRW";account.alias="SYNTHETIC_CASH";
        QVERIFY(vault.addAccount(account,{{"access_key","FAKE_CURRENCY_ACCESS"},{"secret","FAKE_CURRENCY_SECRET"}},StorageMode::EncryptedVault));
        AccountModel accounts(&vault);NoTray tray;CashTransport http;PortfolioController::Factories factories;
        factories.trading=[&](const AccountRecord&){return &http;};factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};PortfolioController portfolio(vault,nullptr,factories);
        LocalWindow window(vault,accounts,nullptr,&tray);window.setChartSettings(&profile);window.setPortfolioController(&portfolio);window.setSecurityStatus(true);window.show();
        auto* total=window.findChild<QLabel*>("portfolioTotal");QVERIFY(total);QTRY_COMPARE_WITH_TIMEOUT(total->text(),QString("1,250,000"),8000);portfolio.setAutomaticRefresh(false);
        const auto diagnostics=[&]{auto result=window.valuationDiagnostics();result.remove("as_of_ms");result.remove("exported_at_ms");return QJsonDocument(result).toJson(QJsonDocument::Compact);};
        QCoreApplication::processEvents();const auto snapshots=portfolio.snapshots();const auto original=diagnostics();QVERIFY(!original.isEmpty());const auto reads=http.reads;
        window.findChild<QPushButton*>("openOptions")->click();auto* currency=window.findChild<QComboBox*>("optionMoneyCurrency");QVERIFY(currency);
        currency->setCurrentIndex(currency->findData("USD"));QCoreApplication::processEvents();
        QCOMPARE(total->text(),QString("1,250,000"));QCOMPARE(window.findChild<QLabel*>("portfolioTotalUnit")->text(),QString("KRW"));
        auto* status=window.findChild<QLabel*>("portfolioCurrencyStatus");QVERIFY2(status,"An unavailable dollar rate must have a small KRW fallback status");QCOMPARE(status->text(),QStringLiteral("환율 불러오는 중"));QVERIFY(status->isVisibleTo(&window));
        QCOMPARE(diagnostics(),original);QCOMPARE(portfolio.snapshots(),snapshots);QCOMPARE(http.reads,reads);
        currency->setCurrentIndex(currency->findData("KRW"));QVERIFY(status->isHidden());QCOMPARE(total->text(),QString("1,250,000"));
        currency->setCurrentIndex(currency->findData("USD"));QCOMPARE(http.reads,reads);
        if(const auto out=qEnvironmentVariable("KEEFI_P3_RENDER_DIR");!out.isEmpty()){
            QVERIFY(QDir().mkpath(out));auto* options=window.findChild<QDialog*>("optionsDialog");QVERIFY(options);options->hide();
            auto* tree=window.findChild<QTreeView*>("portfolioAssets");QVERIFY(tree);
            for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){
                KeefiTheme::instance()->setMode(mode);const auto theme=mode==ThemeMode::Light?"light":"dark";
                for(const auto code:{QString("KRW"),QString("USD")}){
                    currency->setCurrentIndex(currency->findData(code));
                    for(int width:{1440,720}){
                        window.resize(width,1050);QTest::qWait(60);QTRY_VERIFY_WITH_TIMEOUT(!portfolio.dayHistoryBusy(),3000);QCoreApplication::processEvents();
                        const auto stem=QString("/assets-%1-%2-%3").arg(code=="USD"?"usd-requested-no-rate":"krw",theme).arg(width);
                        QVERIFY(window.grab().save(out+stem+".png"));
                    }
                }
                options->show();QCoreApplication::processEvents();
                auto* displayTab=options->findChild<QPushButton*>("optionsDisplayTab");QVERIFY(displayTab);displayTab->click();QCoreApplication::processEvents();
                QVERIFY(options->grab().save(out+QString("/options-display-%1.png").arg(theme)));options->hide();
            }
            QCOMPARE(http.reads,reads);QCOMPARE(portfolio.snapshots(),snapshots);
        }
        window.lockAndClear();QCOMPARE(total->text(),QString::fromUtf8("—"));QVERIFY(status->isHidden());QVERIFY(status->text().isEmpty());
        QCOMPARE(profile.value("display/moneyCurrency/v1").toString(),QString("USD"));
    }
};
QTEST_MAIN(DisplayCurrencyTest)
#include "tst_display_currency.moc"
