#include "keefi/TradingController.h"
#include "keefi/BuildPolicy.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "fixtures/BithumbResponses.h"
#include <QApplication>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <sodium.h>
using namespace keefi;
using namespace keefi::test;
namespace {
class Transport final : public TradingTransport {
public:
    struct Stored { OrderInput input; QString state="wait"; };
    Vault* vault=nullptr; QList<Operation> sent; QMap<QString,Stored> orders;
    bool timeout=false, rulesFail=false, badWatch=false, historyFail=false, changeRemaining=false;
    bool journalBeforeSend=true; int canceled=0; quint64 epoch=0; QJsonObject lastMutation;
    Transport(){ orders["fake-bithumb-order"]={bithumbInput(),"wait"}; orders["fake-bithumb-watch"]={bithumbInput(),"watch"}; }
    void abort() override { ++epoch; }
    int mutations() const { int count=0;for(auto op:sent)if(isMutation(op))++count;return count; }
    void send(quint64 token,const RequestSpec& request) override {
        QVERIFY(NetworkTradingTransport::allowed(request));sent.append(request.operation);
        auto result=bithumbResponse(request);QUrlQuery query(request.url);
        if(request.operation==Operation::OpenOrders){
            QJsonArray rows;const auto state=query.queryItemValue("state");
            for(auto it=orders.cbegin();it!=orders.cend();++it)if(it->state==state)rows.append(bithumbOrder(it.key(),it->state,true,it->input));
            result=badWatch&&state=="watch"?bithumbJson(QJsonObject{{"data",rows},{"has_next",false}}):bithumbPage(rows);
        }
        if((request.operation==Operation::Rules&&rulesFail)||(request.operation==Operation::History&&historyFail))
            result=bithumbJson(QJsonObject{{"error",QJsonObject{{"name","FAKE_UNKNOWN"},{"message","PRIVATE_RAW_ERROR"}}}},500);
        if(isMutation(request.operation)){
            journalBeforeSend=journalBeforeSend&&vault&&!vault->pendingOperations().isEmpty();
            if(timeout){result={};result.timedOut=true;result.networkError=true;}
            else if(request.operation==Operation::PlaceOrder){
                lastMutation=QJsonDocument::fromJson(request.body).object();auto input=bithumbInput();
                input.clientId=lastMutation.value("client_order_id").toString();input.side=lastMutation.value("side")=="bid"?"buy":"sell";
                input.type=lastMutation.value("order_type")=="limit"?"limit":"market";
                input.price=lastMutation.value("price").toString();input.quantity=lastMutation.value("volume").toString();
                if(input.type=="market"&&input.side=="buy"){input.quoteAmount=input.price;input.price.clear();}
                orders["fake-bithumb-new"]={input,"wait"};
            }else if(request.operation==Operation::CancelOrder){
                orders[query.queryItemValue("order_id")].state="cancel";++canceled;
                // An uncertain DELETE receipt is still followed by exact GET observation.
                if(changeRemaining&&canceled==1&&orders.contains("fake-bithumb-second"))orders["fake-bithumb-second"].input.price="100100";
            }
        }
        if(request.operation==Operation::QueryOrder){
            const auto id=query.queryItemValue("uuid"),client=query.queryItemValue("client_order_id");bool found=false;
            for(auto it=orders.cbegin();it!=orders.cend();++it)if((!id.isEmpty()&&it.key()==id)||(id.isEmpty()&&it->input.clientId==client)){
                result=bithumbJson(bithumbOrder(it.key(),it->state,false,it->input));found=true;break;
            }
            if(!found)result=bithumbJson(QJsonObject{{"error",QJsonObject{{"name","order_not_found"},{"message","FAKE_ONLY"}}}},404);
        }
        const auto revision=epoch;QTimer::singleShot(1,this,[this,revision,token,result]{if(revision==epoch)emit finished(token,result);});
    }
};
struct Context {
    QTemporaryDir dir;Vault vault{dir.filePath("vault")};Transport transport;TradingController controller{vault,&transport};QPushButton button;
    Context(){QString error;QVERIFY2(vault.create("Fake Bithumb password! 2026",&error),qPrintable(error));auto account=bithumbAccount();account.id.clear();
        QVERIFY2(vault.addAccount(account,{{"api_key","FAKE_BITHUMB_KEY"},{"secret","FAKE_BITHUMB_SECRET"}},StorageMode::EncryptedVault,&error),qPrintable(error));
        transport.vault=&vault;QVERIFY(controller.selectAccount(vault.accounts().first().id));}
    void connect(bool includeHistory=false){controller.refresh("KRW-BTC",includeHistory);QTRY_VERIFY_WITH_TIMEOUT(!controller.busy()&&controller.connected()&&controller.requestReadiness().ready,10000);}
    OrderInput input(){auto in=bithumbInput();in.clientId.clear();in.startTimeMs=in.endTimeMs=0;return in;}
    void prepare(OrderInput in){QVERIFY(controller.setLimits({"100000","KRW","10"}));controller.setDraft(in);QVERIFY(controller.prepareDraft());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy()&&controller.requestReadiness().ready,6000);
        QVERIFY2(controller.draftReadiness().ready,qPrintable(controller.draftReadiness().message+" / "+controller.status()));}

    void focus(){button.show();button.activateWindow();QApplication::setActiveWindow(&button);button.setFocus();QCoreApplication::processEvents();}
    OrderRecord order(QString id="fake-bithumb-order"){for(const auto& o:controller.openOrders())if(o.orderId==id)return o;return {};}
};
}
class BithumbTradingTest final : public QObject {
    Q_OBJECT
private slots:
    void basicReadAndFailedChanceDoNotClaimTradePermission(){
        Context c;c.transport.historyFail=true;c.connect(true);QVERIFY(c.controller.connected());QCOMPARE(c.controller.openOrders().size(),2);
        QVERIFY(!c.order("fake-bithumb-watch").cancelable);QVERIFY(c.controller.permissionStatus().contains("조회 인증 확인"));
        QVERIFY(c.controller.coverage().contains("조회 실패"));
        QVERIFY(!c.transport.sent.contains(Operation::TestOrder));
        c.transport.rulesFail=true;c.controller.setDraft(c.input());QVERIFY(c.controller.prepareDraft());
        QTRY_VERIFY_WITH_TIMEOUT(!c.controller.busy(),3000);QVERIFY(c.controller.connected());QVERIFY(!c.controller.draftReadiness().ready);
        QCOMPARE(c.transport.mutations(),0);QVERIFY(!c.controller.status().contains("PRIVATE_RAW"));
    }
    void initTestCase(){QVERIFY(sodium_init()>=0);}





    void malformedWatchNeverCompletesWholeScope(){
        Context c;c.transport.badWatch=true;c.controller.refresh("KRW-BTC");QTRY_VERIFY_WITH_TIMEOUT(!c.controller.busy(),5000);
        QVERIFY(!c.controller.connected());QVERIFY(c.controller.openOrders().isEmpty());QCOMPARE(c.transport.mutations(),0);
    }
};
QTEST_MAIN(BithumbTradingTest)
#include "tst_bithumb_trading.moc"
