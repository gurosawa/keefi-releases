#include "keefi/TradingController.h"
#include "keefi/BuildPolicy.h"
#include "keefi/TradingTransport.h"
#include "keefi/OkxOrderEvents.h"
#include "keefi/OkxProtocol.h"
#include "keefi/Vault.h"
#include "fixtures/OkxResponses.h"
#include <QApplication>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <sodium.h>
using namespace keefi;
using namespace keefi::test;
namespace {
class Events final:public OkxOrderEvents{
public:
    bool available=false;quint64 epoch=0;QString scope;
    void start(const AccountRecord&,const CredentialSet&,const QString& s,qint64)override{scope=s;available=true;++epoch;emit stateChanged();}
    void stop()override{available=false;++epoch;emit stateChanged();}
    bool ready()const override{return available;}
    QString status()const override{return available?"가짜 구독 확인":"미연결";}
    quint64 generation()const override{return epoch;}
    void push(QJsonObject raw,QString req,QString outcome,quint64 generation=0){
        raw["reqId"]=req;raw["amendResult"]=outcome;OrderRecord row;QString error;
        QVERIFY2(parseOkxOrder(raw,OkxOrderSource::WebSocket,&row,&error),qPrintable(error));
        emit orderEvent({generation?generation:epoch,scope,row.symbol,row.orderId,req,outcome,row.sourceUpdatedAt,row});
    }
};
class Transport final:public TradingTransport{
public:
    Vault* vault=nullptr;Events* events=nullptr;QList<Operation> sent;QMap<QString,QJsonObject> orders;
    QJsonObject lastMutation;bool timeout=false,reject=false,optionalFail=false,badPages=false,earlyAmend=false;
    bool journalBeforeSend=true;quint64 epoch=0;
    Transport(){orders["100"]=okxOrder();}
    void abort()override{++epoch;}
    int mutations()const{int n=0;for(auto op:sent)if(isMutation(op))++n;return n;}
    void send(quint64 token,const RequestSpec& request)override{
        QVERIFY(NetworkTradingTransport::allowed(request));sent.append(request.operation);
        auto result=okxResponse(request);const QUrlQuery query(request.url);
        if(request.operation==Operation::OpenOrders){QJsonArray rows;for(const auto&r:orders)if(r.value("state")=="live"||r.value("state")=="partially_filled")rows.append(r);
            if(badPages)rows.append(rows.first());result=okxJson(rows);}
        if(optionalFail&&(request.operation==Operation::FundingBalance||request.operation==Operation::History))result=okxJson({},"50000",500);
        if(isMutation(request.operation)){
            journalBeforeSend=journalBeforeSend&&vault&&!vault->pendingOperations().isEmpty();lastMutation=QJsonDocument::fromJson(request.body).object();
            if(timeout){result={};result.timedOut=true;result.networkError=true;}
            else if(reject)result=okxJson({QJsonObject{{"ordId",""},{"clOrdId",lastMutation.value("clOrdId")},{"sCode","51008"},{"sMsg","PRIVATE-SERVER-MESSAGE"}}});
            else if(request.operation==Operation::PlaceOrder){auto row=okxOrder("200");row["clOrdId"]=lastMutation.value("clOrdId");row["px"]=lastMutation.value("px");row["sz"]=lastMutation.value("sz");orders["200"]=row;}
            else if(request.operation==Operation::CancelOrder)orders[lastMutation.value("ordId").toString()]["state"]="canceled";
            else if(request.operation==Operation::AmendOrder){auto& row=orders[lastMutation.value("ordId").toString()];row["px"]=lastMutation.value("newPx");row["sz"]=lastMutation.value("newSz");row["uTime"]=QString::number(QDateTime::currentMSecsSinceEpoch());
                if(earlyAmend&&events)events->push(row,lastMutation.value("reqId").toString(),"0");}
        }
        if(request.operation==Operation::QueryOrder){
            const auto id=query.queryItemValue("ordId");const auto client=query.queryItemValue("clOrdId");QJsonArray rows;
            for(const auto&r:orders)if((!id.isEmpty()&&r.value("ordId")==id)||(id.isEmpty()&&r.value("clOrdId")==client))rows.append(r);
            result=rows.isEmpty()?okxJson({},"51603",200):okxJson(rows);
        }
        const auto generation=epoch;QTimer::singleShot(1,this,[this,generation,token,result]{if(generation==epoch)emit finished(token,result);});
    }
};
struct Context{
    QTemporaryDir directory;Vault vault{directory.filePath("vault")};Transport transport;Events events;TradingController controller{vault,&transport,&events};QPushButton button;
    Context(){QString error;QVERIFY2(vault.create("Fake OKX test password! 2026",&error),qPrintable(error));auto a=okxAccount();a.id.clear();QVERIFY2(vault.addAccount(a,{{"api_key","FAKE_OKX_KEY"},{"secret","FAKE_OKX_SECRET"},{"passphrase"," FAKE passphrase "}},StorageMode::EncryptedVault,&error),qPrintable(error));transport.vault=&vault;transport.events=&events;QVERIFY(controller.selectAccount(vault.accounts().first().id));}
    void connect(bool includeHistory=false){controller.refresh("BTC-USDT",includeHistory);QTRY_VERIFY_WITH_TIMEOUT(!controller.busy()&&controller.connected()&&controller.requestReadiness().ready,10000);}
    OrderInput draft(){auto i=okxInput();i.clientId.clear();return i;}
    void prepare(OrderInput input){QVERIFY(controller.setLimits({"1000","USDT","10"}));controller.setDraft(input);QVERIFY(controller.prepareDraft());QTRY_VERIFY_WITH_TIMEOUT(!controller.busy()&&controller.requestReadiness().ready,12000);QVERIFY2(controller.draftReadiness().ready,qPrintable(controller.draftReadiness().message+" / "+controller.status()));}

    void focus(){button.show();button.activateWindow();QApplication::setActiveWindow(&button);button.setFocus();QCoreApplication::processEvents();}
};
}
class OkxTradingTest final:public QObject{
    Q_OBJECT
private slots:
    void initTestCase(){QVERIFY(sodium_init()>=0);}






    void duplicateScopeNeverBecomesComplete(){Context c;c.transport.badPages=true;c.controller.refresh("BTC-USDT");QTRY_VERIFY_WITH_TIMEOUT(!c.controller.busy(),5000);QVERIFY(!c.controller.connected());QVERIFY(c.controller.coverage().contains("계좌 미체결 · 조회 실패"));QCOMPARE(c.transport.mutations(),0);}
};
QTEST_MAIN(OkxTradingTest)
#include "tst_okx_trading.moc"
