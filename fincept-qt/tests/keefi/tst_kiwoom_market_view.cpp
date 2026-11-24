#include "keefi/KiwoomMarketView.h"
#include "keefi/PortfolioController.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/Vault.h"
#include "keefi/InfoButton.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KiwoomMarketStream.h"
#include "keefi/KiwoomRequestBudget.h"
#include "PublicViewPreview.h"
#include "fixtures/ProviderReadFixture.h"
#include <QComboBox>
#include <QDialog>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTimeZone>
#include <QTreeWidget>
using namespace keefi;
namespace {
QByteArray uiHeader(const RequestSpec& request,const QByteArray& name){
    for(auto it=request.headers.cbegin();it!=request.headers.cend();++it)if(it.key().compare(name,Qt::CaseInsensitive)==0)return it.value();return {};
}
class DisplayHttp final:public TradingTransport {
  public:
    QList<RequestSpec> requests;int aborts=0;
    void abort()override{++aborts;}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(KiwoomMarketProtocol::isRequest(request));QCOMPARE(request.operation,Operation::Reference);requests.append(request);
        const auto api=uiHeader(request,"api-id");const auto body=QJsonDocument::fromJson(request.body).object();QJsonObject response;
        if(api=="usa10098")response={{"list",QJsonArray{QJsonObject{{"stex_tp","ND"},{"stk_cd",body.value("stk_cd")},{"stk_nm","합성 UI 공개 종목"},{"stk_enm","SYNTHETIC UI ONLY"}}}}};
        else if(api=="usa20100")response={{"stex_tp",body.value("stex_tp")},{"stk_cd",body.value("stk_cd")},{"curr_unit","USD"},{"cur_prc","+185.1250"},{"flu_rt","-2.50"}};
        else QFAIL("Unexpected display request in synthetic UI pipeline");
        response["return_code"]=0;HttpResult result;result.status=200;result.headers={{"api-id",api},{"cont-yn","N"}};result.body=QJsonDocument(response).toJson(QJsonDocument::Compact);
        QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});
    }
    int prices()const{int count=0;for(const auto& request:requests)if(uiHeader(request,"api-id")=="usa20100")++count;return count;}
};
class DisplaySocket final:public KiwoomMarketSocket {
  public:
    QList<quint64> opens,logins,closes;QList<QByteArray> controls;int aborts=0;bool borrowedExistingToken=false;
    void open(quint64 generation,const QString& country)override{QCOMPARE(country,QString("US"));opens.append(generation);}
    void sendLogin(quint64 generation,QByteArrayView token)override{logins.append(generation);borrowedExistingToken=token==QByteArrayView("fake-token.only_for_test");}
    void sendControl(quint64,const QByteArray& bytes)override{controls.append(bytes);}
    void close(quint64 generation)override{closes.append(generation);}
    void abort()override{++aborts;}
    void frame(quint64 generation,const QByteArray& bytes){emit received(generation,bytes);}
};
QByteArray uiRealtime(const QString& price="186.3750"){return QJsonDocument(QJsonObject{{"trnm","REAL"},{"data",QJsonArray{QJsonObject{{"type","FE"},{"item","UISYN"},{"stexTp","ND"},
    {"values",QJsonObject{{"10",price},{"12","-9.99"},{"20","071234"},{"22","20261023"},{"51020","171234"}}}}}}}).toJson(QJsonDocument::Compact);}
}
class KiwoomMarketViewTest final:public QObject {
    Q_OBJECT
  private slots:
    void actualPublicSourcePipelineKeepsTypedInkClocksAndPrivateClearSeparate();
    void publicScopeAndUnknownUtcInformationOpenFromMouseAndKeyboard(){
        KiwoomMarketView view(nullptr,nullptr);view.resize(820,700);view.show();QVERIFY(!view.privateEnabled());QVERIFY(view.selectedConnectionId().isEmpty());
        auto* title=view.findChild<QLabel*>("kiwoomMarketTitle");QVERIFY(title);QVERIFY(title->text().contains(QStringLiteral("키움")));QCOMPARE(title->textFormat(),Qt::PlainText);
        auto* info=view.findChild<InfoButton*>("kiwoomMarketInfo");QVERIFY(info);info->setFocus();QTest::keyClick(info,Qt::Key_Space);QTRY_VERIFY(info->informationOpen());auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);
        const auto text=dialog->findChild<QPlainTextEdit*>("infoPublicText")->toPlainText();QVERIFY(text.contains("Nasdaq TotalView"));QVERIFY(text.contains(QStringLiteral("원천 UTC 시각 미제공")));QVERIFY(text.contains("200"));QVERIFY(text.contains(QStringLiteral("계좌·연결")));QVERIFY(text.contains(QStringLiteral("완료 봉")));QVERIFY(text.contains(QStringLiteral("알림")));
        QVERIFY2(!text.contains("://"),qPrintable(text));
        auto* privateText=dialog->findChild<QPlainTextEdit*>("infoPrivateText");QVERIFY(!privateText||privateText->isHidden()||privateText->toPlainText().isEmpty());QTest::keyClick(dialog,Qt::Key_Escape);QTRY_VERIFY(!info->informationOpen());QTest::mouseClick(info,Qt::LeftButton);QTRY_VERIFY(info->informationOpen());QTest::keyClick(info->findChild<QDialog*>("infoDialog"),Qt::Key_Escape);
    }
    void defaultPrivateGateAndLateSignalsNeverRehydrateExistingConnection(){
        QTemporaryDir directory;QVERIFY(directory.isValid());Vault vault(directory.filePath("synthetic-vault"));QVERIFY(vault.create("SyntheticUiPassword123!"));
        test::ReadFixture fixture("kiwoom");fixture.account.alias="SYNTHETIC_PRIVATE_LINK_CANARY";QVERIFY(vault.addAccount(fixture.account,fixture.credentials,StorageMode::EncryptedVault));
        test::ProviderReadTransport http;http.provider="kiwoom";PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &http;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);portfolio.stopReads();portfolio.setAutomaticRefresh(false);QVERIFY(!portfolio.connections().isEmpty());const auto id=portfolio.connections().first().account.id;auto* existing=portfolio.broker(id);QVERIFY(existing);QTRY_VERIFY(!existing->busy());
        const auto baseline=http.sent.size();KiwoomMarketView view(&portfolio,nullptr);view.resize(900,700);view.show();auto* connections=view.findChild<QComboBox*>("kiwoomMarketConnections");QVERIFY(connections);
        QVERIFY(!view.privateEnabled());QCOMPARE(connections->count(),0);QVERIFY(view.selectedConnectionId().isEmpty());QCOMPARE(http.sent.size(),baseline);
        view.setPrivateEnabled(true);QVERIFY(view.privateEnabled());QTRY_COMPARE(connections->count(),1);QCOMPARE(view.selectedConnectionId(),id);QCOMPARE(portfolio.broker(id),existing);QCOMPARE(http.sent.size(),baseline);
        auto* info=view.findChild<InfoButton*>("kiwoomMarketInfo");QVERIFY(info);QTest::mouseClick(info,Qt::LeftButton);QTRY_VERIFY(info->informationOpen());
        auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);QPointer<QPlainTextEdit> privateText=dialog->findChild<QPlainTextEdit*>("infoPrivateText");QVERIFY(privateText);QVERIFY(privateText->toPlainText().contains("SYNTHETIC_PRIVATE_LINK_CANARY"));
        view.clearPrivate();QVERIFY(!view.privateEnabled());QVERIFY(view.selectedConnectionId().isEmpty());QCOMPARE(connections->count(),0);QVERIFY(!info->informationOpen());if(privateText)QVERIFY(privateText->toPlainText().isEmpty());
        emit portfolio.changed();QCoreApplication::processEvents();QVERIFY(view.selectedConnectionId().isEmpty());QCOMPARE(connections->count(),0);QCOMPARE(http.sent.size(),baseline);
        for(auto* label:view.findChildren<QLabel*>()){QVERIFY(!label->text().contains("SYNTHETIC_PRIVATE_LINK_CANARY"));QVERIFY(!label->toolTip().contains("SYNTHETIC_PRIVATE_LINK_CANARY"));}
    }
    void rootReaffirmedCountryRestoresDisplayAfterKeyboardRequest(){
        QTemporaryDir directory;QVERIFY(directory.isValid());Vault vault(directory.filePath("synthetic-vault"));QVERIFY(vault.create("SyntheticUiPassword123!"));
        test::ReadFixture fixture("kiwoom");QVERIFY(vault.addAccount(fixture.account,fixture.credentials,StorageMode::EncryptedVault));
        test::ProviderReadTransport http;http.provider="kiwoom";PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &http;};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);portfolio.stopReads();portfolio.setAutomaticRefresh(false);QVERIFY(!portfolio.connections().isEmpty());
        auto* existing=portfolio.broker(portfolio.connections().first().account.id);QVERIFY(existing);QTRY_VERIFY(!existing->busy());const auto baseline=http.sent.size();
        KiwoomMarketView view(&portfolio,nullptr);view.resize(820,700);view.show();view.setPrivateEnabled(true);
        auto* country=view.findChild<QComboBox*>("kiwoomMarketCountry");QVERIFY(country);QVERIFY(country->isEnabled());
        QSignalSpy requests(&view,&KiwoomMarketView::activeCountryRequested);view.setActiveCountryDisplay("US");QCOMPARE(country->currentData().toString(),QString("US"));QCOMPARE(requests.count(),0);
        country->setFocus();QTest::keyClick(country,Qt::Key_Up);QCOMPARE(country->currentData().toString(),QString("KR"));QCOMPARE(requests.count(),1);QCOMPARE(requests.first().first().toString(),QString("KR"));
        // Root retains the single collector's already-approved US context.
        // A repeated setter must restore the presentation without another request.
        view.setActiveCountryDisplay("US");QCOMPARE(country->currentData().toString(),QString("US"));QCOMPARE(requests.count(),1);QCOMPARE(http.sent.size(),baseline);
    }
};
void KiwoomMarketViewTest::actualPublicSourcePipelineKeepsTypedInkClocksAndPrivateClearSeparate(){
    QTemporaryDir directory;QVERIFY(directory.isValid());Vault vault(directory.filePath("synthetic-pipeline-vault"));QVERIFY(vault.create("SyntheticUiPassword123!"));
    test::ReadFixture fixture("kiwoom");fixture.account.alias="SYNTHETIC_UI_PRIVATE_LINK";QVERIFY(vault.addAccount(fixture.account,fixture.credentials,StorageMode::EncryptedVault));
    test::ProviderReadTransport accountHttp;accountHttp.provider="kiwoom";PortfolioController::Factories factories;factories.prices=[] {return static_cast<TradingTransport*>(nullptr);};factories.trading=[&](const AccountRecord&){return &accountHttp;};
    PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);portfolio.stopReads();portfolio.setAutomaticRefresh(false);QVERIFY(!portfolio.connections().isEmpty());
    auto* existing=portfolio.broker(portfolio.connections().first().account.id);QVERIFY(existing);QTRY_VERIFY(!existing->busy());
    // Explicit synthetic authentication is a fixture precondition, never a View/Source action.
    if(!existing->authenticated())existing->authenticate();QTRY_VERIFY_WITH_TIMEOUT(existing->authenticated()&&!existing->busy(),5000);
    const auto accountReads=accountHttp.sent;QCOMPARE(accountReads.count(Operation::Authenticate),1);
    qint64 monotonic=0,utc=QDateTime::currentMSecsSinceEpoch();DisplayHttp displayHttp;auto socket=std::make_unique<DisplaySocket>();auto* wire=socket.get();
    KiwoomMarketStream source(nullptr,&displayHttp,std::move(socket),[&]{return monotonic;},[&]{return utc;},nullptr,KiwoomRequestBudget::shared());
    auto view=std::make_unique<KiwoomMarketView>(&portfolio,&source);auto* policy=new MovementColorPolicy(view.get());view->resize(900,700);view->show();auto* rows=view->findChild<QTreeWidget*>("kiwoomMarketQuotes");QVERIFY(rows);QCOMPARE(rows->topLevelItemCount(),0);
    view->setPrivateEnabled(true);QCOMPARE(view->selectedConnectionId(),portfolio.connections().first().account.id);QCOMPARE(rows->topLevelItemCount(),0);QCOMPARE(displayHttp.requests.size(),0);QCOMPARE(accountHttp.sent,accountReads);
    // This fixture is the sole arbiter, mirroring LocalWindow; the View never calls these setters.
    source.setBroker(portfolio.broker(view->selectedConnectionId()));source.setTargets({{{"stock","US","UISYN"},"",0,true}});source.setActiveCountry("US");view->setActiveCountryDisplay("US");
    const auto step=[&]{monotonic+=100;utc+=100;source.tick();QCoreApplication::processEvents();};
    QTRY_VERIFY_WITH_TIMEOUT(([&]{step();return source.instruments().size()==1&&wire->opens.size()==1&&source.quotes().size()==1;})(),6000);
    const auto instrument=source.instruments().first();QCOMPARE(instrument.identity.key(),QString("stock:US:UISYN"));QCOMPARE(instrument.currency,QString("USD"));QVERIFY(instrument.verified);QCOMPARE(instrument.metadataApiId,QString("usa10098"));QCOMPARE(instrument.currencyApiId,QString("usa20100"));
    const auto seed=source.quotes().first();QCOMPARE(seed.price,QString("185.125"));QCOMPARE(seed.direction,MovementDirection::Down);QCOMPARE(seed.changeText,QString("-2.5%"));QVERIFY(!seed.realtime);QCOMPARE(seed.evidence.sourceTimeMs,0LL);QVERIFY(seed.evidence.receivedAtMs>0);
    QTRY_COMPARE(rows->topLevelItemCount(),1);auto* observedRow=rows->topLevelItem(0);const auto restLabel=observedRow->text(3),restPrice=observedRow->text(1),restChange=observedRow->text(2);const auto normalInk=observedRow->foreground(2).color();rows->setCurrentItem(observedRow);
    QCOMPARE(observedRow->data(0,Qt::UserRole).toString(),QString("stock:US:UISYN|ND"));
    QVERIFY2(!observedRow->text(0).contains("stock:"),qPrintable(observedRow->text(0)));QVERIFY(observedRow->text(0).contains("UISYN"));
    const auto restReceipt=seed.evidence.receivedAtMs;const int requestsBeforePalette=displayHttp.requests.size(),controlsBeforePalette=wire->controls.size();policy->setScheme(MovementScheme::KoreanRedBlue);const auto koreanInk=observedRow->foreground(2).color();
    QCOMPARE(source.quotes().first().evidence.receivedAtMs,restReceipt);QCOMPARE(displayHttp.requests.size(),requestsBeforePalette);QCOMPARE(wire->controls.size(),controlsBeforePalette);QCOMPARE(accountHttp.sent,accountReads);
    const auto generation=wire->opens.first();emit wire->connected(generation);QCOMPARE(wire->logins.size(),1);QVERIFY(wire->borrowedExistingToken);QCOMPARE(wire->controls.size(),0);
    wire->frame(generation,R"({"trnm":"LOGIN","return_code":0})");QCOMPARE(wire->controls.size(),1);wire->frame(generation,uiRealtime());QVERIFY(!source.quotes().first().realtime);
    wire->frame(generation,R"({"trnm":"REG","return_code":0})");QTRY_VERIFY_WITH_TIMEOUT(([&]{step();return displayHttp.prices()>=2&&source.quotes().size()==1&&source.quotes().first().evidence.receivedAtMs>restReceipt;})(),6000);
    monotonic+=1;utc+=1;source.tick();wire->frame(generation,uiRealtime());QTRY_VERIFY(source.quotes().size()==1&&source.quotes().first().realtime);
    const auto current=source.quotes().first();QCOMPARE(current.price,QString("186.375"));QCOMPARE(current.currency,QString("USD"));QCOMPARE(current.evidence.sourceTimeMs,0LL);QCOMPARE(current.evidence.receivedAtMs,utc);QCOMPARE(current.rawDate,QString("20261023"));QCOMPARE(current.rawTime,QString("071234"));QCOMPARE(current.rawLocalTime,QString("171234"));QVERIFY(current.changeText.isEmpty());QCOMPARE(current.direction,MovementDirection::Unknown);
    QTRY_COMPARE(rows->topLevelItem(0)->text(1),QString("186.375 USD"));QCOMPARE(rows->topLevelItem(0),observedRow);QCOMPARE(rows->currentItem(),observedRow);QCOMPARE(observedRow->text(2),QString{});QCOMPARE(observedRow->foreground(2).color(),policy->foreground(MovementDirection::Unknown));
    const auto readsBeforeInfo=displayHttp.requests.size(),controlsBeforeInfo=wire->controls.size();auto* info=qobject_cast<InfoButton*>(rows->itemWidget(observedRow,5));QVERIFY(info);info->setFocus();QTest::keyClick(info,Qt::Key_Space);QTRY_VERIFY(info->informationOpen());
    QPointer<QDialog> dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);const auto text=dialog->findChild<QPlainTextEdit*>("infoPublicText")->toPlainText();QVERIFY(text.contains(QStringLiteral("원천 UTC 시각 미제공")));QVERIFY(text.contains("20261023"));QVERIFY(text.contains("071234"));QVERIFY(text.contains("171234"));QVERIFY(!text.contains("://"));QVERIFY(!text.contains("stock:"));QVERIFY(!dialog->windowTitle().contains("stock:"));QVERIFY(!text.contains("SYNTHETIC_UI_PRIVATE_LINK"));QVERIFY(!text.contains("public:kiwoom:"));
    QCOMPARE(source.quotes().first().identity,current.identity);QCOMPARE(source.quotes().first().evidence,current.evidence);QVERIFY(current.evidence.url.contains("api.kiwoom.com"));
    QVERIFY(observedRow->toolTip(1).contains(QStringLiteral("키움증권")));QVERIFY(!observedRow->toolTip(1).contains('\n'));
    for(int column=0;column<rows->columnCount();++column)for(const auto& display:{observedRow->text(column),observedRow->toolTip(column),observedRow->data(column,Qt::AccessibleTextRole).toString(),observedRow->data(column,Qt::AccessibleDescriptionRole).toString()}){QVERIFY(!display.contains("://"));QVERIFY(!display.contains("stock:"));}
    QTest::keyClick(dialog,Qt::Key_Escape);QTRY_VERIFY(!info->informationOpen());QTest::mouseClick(info,Qt::LeftButton);QTRY_VERIFY(info->informationOpen());dialog=info->findChild<QDialog*>("infoDialog");
    QCOMPARE(displayHttp.requests.size(),readsBeforeInfo);QCOMPARE(wire->controls.size(),controlsBeforeInfo);QCOMPARE(accountHttp.sent,accountReads);
    const auto liveLabel=observedRow->text(3);QVERIFY(test::savePublicViewPreview(*view,"c11-kiwoom-market"));
    monotonic+=1;utc+=1;source.tick();wire->frame(generation,uiRealtime("12345.6250"));
    QTRY_COMPARE(source.quotes().first().price,QString("12345.625"));const auto large=source.quotes().first();
    QTRY_COMPARE(observedRow->text(1),QString("12,345.625 USD"));
    const auto readsBeforeGrouping=displayHttp.requests.size(),controlsBeforeGrouping=wire->controls.size();emit source.changed();
    QCOMPARE(source.quotes().first().price,large.price);QCOMPARE(source.quotes().first().identity,large.identity);QCOMPARE(source.quotes().first().evidence,large.evidence);
    QCOMPARE(displayHttp.requests.size(),readsBeforeGrouping);QCOMPARE(wire->controls.size(),controlsBeforeGrouping);QCOMPARE(accountHttp.sent,accountReads);
    const auto closes=wire->closes.size();const auto aborts=wire->aborts;view->clearPrivate();QCOMPARE(rows->topLevelItemCount(),0);QVERIFY(dialog.isNull());QVERIFY(!view->privateEnabled());
    emit portfolio.changed();emit source.changed();QCoreApplication::processEvents();QCOMPARE(rows->topLevelItemCount(),0);QVERIFY(view->selectedConnectionId().isEmpty());QCOMPARE(source.quotes().size(),1);QCOMPARE(wire->closes.size(),closes);QCOMPARE(wire->aborts,aborts);
    for(auto* body:view->findChildren<QPlainTextEdit*>()){QVERIFY(!body->toPlainText().contains("UISYN"));QVERIFY(!body->toPlainText().contains("SYNTHETIC_UI_PRIVATE_LINK"));}
    view->hide();view.reset();QCOMPARE(source.quotes().size(),1);QCOMPARE(wire->closes.size(),closes);QCOMPARE(wire->aborts,aborts);QCOMPARE(accountHttp.sent,accountReads);
    // Assertions after all producer prerequisites distinguish a UI failure from setup.
    QCOMPARE(restPrice,QString("185.125 USD"));QCOMPARE(restChange,QString("▼-2.5%"));QCOMPARE(normalInk,themeColors().downText);QCOMPARE(koreanInk,themeColors().koreanDownText);
    QVERIFY(restLabel.contains(QStringLiteral("키움 미국 REST 현재가")));QVERIFY(!restLabel.contains("Nasdaq TotalView"));QVERIFY(liveLabel.contains(QStringLiteral("Nasdaq TotalView 실시간")));
}
QTEST_MAIN(KiwoomMarketViewTest)
#include "tst_kiwoom_market_view.moc"
