#include "keefi/FuturesTradingController.h"
#include "keefi/BuildPolicy.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include "fixtures/BinanceUsdmResponses.h"
#include <QApplication>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
using namespace keefi;
using namespace keefi::test;
namespace {
class Transport final : public TradingTransport {
public:
    Vault* vault=nullptr;QList<Operation> sent;QMap<QString,OrderInput> orders;QSet<QString> canceled;
    bool localBlockMutation=false,timeout=false,badAlgo=false,journalBeforeSend=true,amendEvidence=true;quint64 epoch=0;
    QMap<QString,OrderInput> conditionalOrders;bool conditionalAcceptedOnTimeout=false,conditionalFinished=false,mismatchConditional=false;
    QString algoStatus="NEW";QJsonObject amendment;qint64 originalUpdate=QDateTime::currentMSecsSinceEpoch();
    Transport(){orders["100"]=usdmInput();}
    void abort() override {++epoch;}
    int mutations()const{int n=0;for(auto op:sent)if(isMutation(op))++n;return n;}
    void send(quint64 token,const RequestSpec& request) override {
        QVERIFY2(NetworkTradingTransport::allowed(request),qPrintable(request.url.path()));sent.append(request.operation);
        if(isMutation(request.operation)&&localBlockMutation){
            journalBeforeSend=vault&&!vault->pendingOperations().isEmpty();
            HttpResult result;result.status=429;result.localRetryAfterSeconds=90;result.headers["Retry-After"]="90";
            QTimer::singleShot(0,this,[this,token,result]{emit finished(token,result);});return;
        }
        auto response=usdmResponse(request);const QUrlQuery query=request.method=="GET"||request.method=="DELETE"?QUrlQuery(request.url):QUrlQuery(QString::fromUtf8(request.body));
        if(request.operation==Operation::OpenOrders){QJsonArray rows;for(auto it=orders.cbegin();it!=orders.cend();++it)if(!canceled.contains(it.key()))rows.append(usdmOrder(it.key(),"NEW",it.value(),"0",originalUpdate));response=usdmJson(rows);}
        if(request.operation==Operation::AlgoOpenOrders)response=badAlgo?usdmJson(QJsonObject{{"unexpected",true}}):usdmJson(algoStatus=="NEW"?QJsonArray{usdmAlgo()}:QJsonArray{});
        if(isMutation(request.operation)){
            journalBeforeSend=journalBeforeSend&&vault&&!vault->pendingOperations().isEmpty();
            if(request.operation==Operation::PlaceOrder&&request.url.path()=="/fapi/v1/algoOrder"&&(!timeout||conditionalAcceptedOnTimeout)){
                auto i=usdmConditionalInput();i.clientAlgoId=query.queryItemValue("clientAlgoId");i.side=query.queryItemValue("side").toLower();i.type=query.queryItemValue("type").toLower();
                i.price=query.queryItemValue("price");i.quantity=query.queryItemValue("quantity");i.futures->triggerPrice=query.queryItemValue("triggerPrice");
                i.futures->workingType=query.queryItemValue("workingType");conditionalOrders["300"]=i;}
            if(timeout){response={};response.timedOut=response.networkError=true;}
            else if(request.operation==Operation::PlaceOrder&&request.url.path()=="/fapi/v1/algoOrder")response=usdmJson(usdmConditionalOrder(conditionalOrders["300"]));
            else if(request.operation==Operation::PlaceOrder){auto in=usdmInput();in.clientId=query.queryItemValue("newClientOrderId");in.price=query.queryItemValue("price");in.quantity=query.queryItemValue("quantity");
                orders["200"]=in;response=usdmJson(usdmOrder("200","NEW",in));}
            else if(request.operation==Operation::CancelOrder){canceled.insert(query.queryItemValue("orderId"));response=usdmJson(usdmOrder(query.queryItemValue("orderId"),"CANCELED",orders.value(query.queryItemValue("orderId"))));}
            else if(request.operation==Operation::AlgoCancelOrder)algoStatus="CANCELED";
            else if(request.operation==Operation::AmendOrder){const auto id=query.queryItemValue("orderId");auto& in=orders[id];
                amendment={{"orderId",id},{"symbol",in.symbol},{"time",QString::number(QDateTime::currentMSecsSinceEpoch())},
                    {"amendment",QJsonObject{{"modifyId",query.queryItemValue("modifyId")},{"price",QJsonObject{{"before",in.price},{"after",query.queryItemValue("price")}}},
                        {"origQty",QJsonObject{{"before",in.quantity},{"after",query.queryItemValue("quantity")}}}}}};
                in.price=query.queryItemValue("price");in.quantity=query.queryItemValue("quantity");response=usdmJson(usdmOrder(id,"NEW",in));}
        }
        if(request.operation==Operation::QueryOrder){bool found=false;for(auto it=orders.cbegin();it!=orders.cend();++it)
            if((query.hasQueryItem("orderId")&&it.key()==query.queryItemValue("orderId"))||(!query.hasQueryItem("orderId")&&it->clientId==query.queryItemValue("origClientOrderId"))){
                response=usdmJson(usdmOrder(it.key(),canceled.contains(it.key())?"CANCELED":"NEW",it.value()));found=true;break;}
            if(!found)response=usdmJson(QJsonObject{{"code","-2013"},{"msg","FAKE_ONLY"}},400);}
        if(request.operation==Operation::AlgoQueryOrder){
            if(query.queryItemValue("algoId")=="100")response=usdmJson(usdmAlgo("100",algoStatus));
            else{bool found=false;for(auto it=conditionalOrders.cbegin();it!=conditionalOrders.cend();++it)
                if((query.hasQueryItem("algoId")&&it.key()==query.queryItemValue("algoId"))||(!query.hasQueryItem("algoId")&&it->clientAlgoId==query.queryItemValue("clientAlgoId"))){
                    auto row=usdmConditionalOrder(it.value(),it.key(),conditionalFinished?"FINISHED":"NEW",conditionalFinished?QString("999"):QString{});
                    if(mismatchConditional)row["price"]="100";response=usdmJson(row);found=true;break;}
                if(!found)response=usdmJson(QJsonObject{{"code","-2013"},{"msg","FAKE_ONLY"}},400);}}
        if(request.operation==Operation::AmendHistory)response=usdmJson(amendEvidence&&!amendment.isEmpty()?QJsonArray{amendment}:QJsonArray{});
        const auto generation=epoch;QTimer::singleShot(1,this,[this,token,generation,response]{if(epoch==generation)emit finished(token,response);});
    }
};
struct Context {
    QTemporaryDir dir;Vault vault{dir.filePath("fake-vault")};Transport transport;FuturesTradingController controller{vault,&transport};QPushButton button;
    Context(){QString error;QVERIFY2(vault.create("Fake Futures Password! 2026",&error),qPrintable(error));auto a=usdmAccount();a.id.clear();
        QVERIFY2(vault.addAccount(a,{{"api_key","FakeFuturesKey123"},{"secret","FakeFuturesSecret123"}},StorageMode::EncryptedVault,&error),qPrintable(error));
        transport.vault=&vault;QVERIFY(controller.selectAccount(vault.accounts().first().id));}
    void connect(){controller.refresh("BTCUSDT");QTRY_VERIFY_WITH_TIMEOUT(!controller.busy()&&controller.connected()&&controller.requestReadiness().ready,7000);}
    OrderInput input(){auto i=usdmInput();i.clientId.clear();i.startTimeMs=i.endTimeMs=0;return i;}
    void focus(){button.show();button.activateWindow();QApplication::setActiveWindow(&button);button.setFocus();QCoreApplication::processEvents();}
    void prepare(const OrderInput& i,bool test=true){QVERIFY(controller.setLimits({"1000","USDT","5"}));controller.setDraft(i);QVERIFY(controller.prepareDraft());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy()&&controller.requestReadiness().ready,9000);
        QVERIFY2(controller.draftReadiness().ready,qPrintable(controller.draftReadiness().message+" / "+controller.status()));
        Q_UNUSED(test);
        }

    OrderRecord order(OrderFamily family){for(const auto& o:controller.openOrders())if(o.futures&&o.futures->family==family)return o;return {};}
};
}
class FuturesTradingTest final : public QObject {
    Q_OBJECT
private slots:
    void readScopeAndDraftChangesPreserveInputWithoutWrites(){Context c;c.connect();QCOMPARE(c.controller.openOrders().size(),2);QCOMPARE(c.controller.positions().size(),1);
        QSignalSpy revoked(&c.controller,&FuturesTradingController::intentRevoked);auto in=c.input();c.controller.setDraft(in);QVERIFY(c.controller.setLimits({"1000","USDT","5"}));
        QCOMPARE(revoked.size(),0);QCOMPARE(c.transport.mutations(),0);c.transport.badAlgo=true;c.controller.refresh("BTCUSDT");QTRY_VERIFY_WITH_TIMEOUT(!c.controller.busy(),7000);
        QVERIFY(!c.controller.connected());QCOMPARE(c.transport.mutations(),0);}







};
QTEST_MAIN(FuturesTradingTest)
#include "tst_futures_trading.moc"
