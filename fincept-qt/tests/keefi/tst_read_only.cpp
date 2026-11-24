#include "keefi/BuildPolicy.h"
#include "keefi/LocalWindow.h"
#include "keefi/TradingController.h"
#include "keefi/FuturesTradingController.h"
#include "keefi/UpbitAdapter.h"
#include "keefi/BinanceAdapter.h"
#include "keefi/BinanceUsdmAdapter.h"
#include "keefi/BithumbAdapter.h"
#include "keefi/OkxAdapter.h"
#include "keefi/TradingTransport.h"
#include "keefi/SolanaReadRpc.h"
#include "keefi/Vault.h"
#include "keefi/PortfolioController.h"
#include "keefi/SolanaTransactionCodec.h"
#include "fixtures/ProviderReadFixture.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/TradingRequestLimits.h"
#include <QSignalSpy>
#include <QDateTime>
#include <QListView>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace keefi;
Q_DECLARE_METATYPE(keefi::RequestSpec)

template<class T> concept HasPlaceOrder = requires { &T::placeOrder; };
template<class T> concept HasCancelOrder = requires { &T::cancelOrder; };
template<class T> concept HasAmendOrder = requires { &T::amendOrder; };
template<class T> concept HasLiveEnabledSetter = requires { &T::setLiveEnabled; };
template<class T> concept HasTransactionDecoder = requires { &T::decode; };
template<class T> concept HasTransactionSigner = requires { &T::verifySigned; };
static_assert(!HasPlaceOrder<TradingController> && !HasCancelOrder<TradingController> && !HasAmendOrder<TradingController> && !HasLiveEnabledSetter<TradingController>);
static_assert(!HasPlaceOrder<BrokerTradingController> && !HasCancelOrder<BrokerTradingController> && !HasAmendOrder<BrokerTradingController> && !HasLiveEnabledSetter<BrokerTradingController>);
static_assert(!HasPlaceOrder<FuturesTradingController> && !HasCancelOrder<FuturesTradingController> && !HasAmendOrder<FuturesTradingController> && !HasLiveEnabledSetter<FuturesTradingController>);
static_assert(!HasTransactionDecoder<SolanaTransactionCodec> && !HasTransactionSigner<SolanaTransactionCodec>);

class FakeAssetTransport final : public TradingTransport {
public:
    int sent = 0;
    void abort() override {}
    void send(quint64 token, const RequestSpec& request) override {
        QVERIFY(request.operation == Operation::Time || request.operation == Operation::Account);
        QVERIFY(NetworkTradingTransport::allowed(request));
        ++sent;
        HttpResult result; result.status = 200;
        result.body = request.operation == Operation::Time
            ? QJsonDocument(QJsonObject{{"serverTime", QString::number(QDateTime::currentMSecsSinceEpoch())}}).toJson()
            : QByteArray(R"({"canTrade":true,"accountType":"SPOT","balances":[{"asset":"BTC","free":"1","locked":"0.5"}]})");
        QTimer::singleShot(0, this, [this, token, result] { emit finished(token, result); });
    }
};

class ReadOnlyTest final : public QObject {
    Q_OBJECT
private slots:
    void accountScreenRecoversFailedBrokerAuthentication_data() {
        QTest::addColumn<QString>("provider"); QTest::addColumn<bool>("expired");
        for(const auto& provider:QStringList{"toss","kiwoom"})for(bool expired:{false,true})
            QTest::newRow(qPrintable(provider+(expired?"-expired":"-initial-failure")))<<provider<<expired;
    }
    void accountScreenRecoversFailedBrokerAuthentication() {
        QFETCH(QString,provider);QFETCH(bool,expired);
        test::ProviderReadTransport transport;transport.provider=provider;
        bool reject=!expired;
        transport.transform=[&](const RequestSpec& r,HttpResult& response){
            if(reject&&(r.operation==Operation::Authenticate||r.operation==Operation::Account||r.operation==Operation::Holdings)){
                response.status=401;response.body="{}";
            }
        };
        QTemporaryDir dir; Vault vault(dir.filePath("vault"));
        QVERIFY(vault.create("Fake Read Only Recovery Password 2026!"));
        test::ReadFixture fixture(provider);
        QVERIFY(vault.addAccount(fixture.account,fixture.credentials,StorageMode::EncryptedVault));
        const auto id=vault.accounts().first().id;
        PortfolioController::Factories factories;
        factories.trading=[&](const AccountRecord&){return &transport;};
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);
        AccountModel accounts(&vault);LocalWindow window(vault,accounts);
        window.setSecurityStatus(true,"FAKE_READY");window.setPortfolioController(&portfolio);
        auto* list=window.findChild<QListView*>("accountList");QVERIFY(list);list->setCurrentIndex(accounts.index(0));
        if(expired){
            QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections().first().state=="complete",7000);
            reject=true;QTest::qWait(1100);portfolio.refreshConnection(id);
            QTRY_VERIFY_WITH_TIMEOUT(!portfolio.broker(id)->authenticated()&&!portfolio.connections().first().active,7000);
        }else QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections().first().state=="authentication_required",3000);
        QCOMPARE(transport.sent.count(Operation::Authenticate),1);
        portfolio.refreshAll();QCOMPARE(transport.sent.count(Operation::Authenticate),1);
        auto* reconnect=window.findChild<QPushButton*>("reconnectReadAccount");
        QVERIFY2(reconnect,"Account management must provide recovery when the trading window is unavailable");
        reject=false;QTRY_VERIFY(reconnect->isEnabled());reconnect->click();reconnect->click();
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections().first().state=="complete",7000);
        QVERIFY(!portfolio.snapshots().isEmpty());QCOMPARE(transport.sent.count(Operation::Authenticate),2);
        QVERIFY(!window.findChild<QPushButton*>("openTrading"));
        vault.lock();QVERIFY(!reconnect->isEnabled());QVERIFY(!portfolio.reconnectConnection(id));
    }
    void providerReadsKeepLastSnapshotOnFailure_data() {
        QTest::addColumn<QString>("provider");QTest::addColumn<bool>("unauthorized");
        for(const auto& provider:QStringList{"binance","binance-usdm","upbit","okx","bithumb","toss","kiwoom"}){
            QTest::newRow(qPrintable(provider+"-malformed"))<<provider<<false;
            if(!QStringList{"toss","kiwoom"}.contains(provider))QTest::newRow(qPrintable(provider+"-401"))<<provider<<true;
        }
    }
    void providerReadsKeepLastSnapshotOnFailure() {
        QFETCH(QString,provider);QFETCH(bool,unauthorized);test::ReadFixture fixture(provider);
        QTemporaryDir dir;Vault vault(dir.filePath("vault"));QVERIFY(vault.create("Fake read-only vault! 2026"));
        QVERIFY(vault.addAccount(fixture.account,fixture.credentials,StorageMode::EncryptedVault));
        const auto id=vault.accounts().first().id;
        test::ProviderReadTransport transport;transport.provider=provider;
        PortfolioController::Factories factories;factories.trading=[&](const AccountRecord&){return &transport;};
        factories.prices=[]{return static_cast<TradingTransport*>(nullptr);};
        PortfolioController portfolio(vault,nullptr,factories);portfolio.setAutomaticRefresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections().first().state=="complete",7000);
        QVERIFY(!portfolio.snapshots().isEmpty());const auto before=portfolio.snapshots().first();
        QVERIFY(!before.components.isEmpty());
        transport.transform=[&](const RequestSpec& r,HttpResult& response){
            if(r.operation==Operation::Account||r.operation==Operation::Holdings){response.status=unauthorized?401:200;response.body="{invalid-json";}
        };
        QTest::qWait(1100);portfolio.refreshConnection(id);
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections().first().state=="failed",7000);
        QCOMPARE(portfolio.snapshots().first().id,before.id);
        QCOMPARE(portfolio.snapshots().first().components.first().quantity,before.components.first().quantity);
        transport.transform={};QTest::qWait(1100);
        if(unauthorized){QVERIFY(portfolio.canReconnectConnection(id));QVERIFY(portfolio.reconnectConnection(id));QVERIFY(!portfolio.reconnectConnection(id));}
        else portfolio.refreshConnection(id);
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.connections().first().state=="complete",7000);
        QVERIFY(portfolio.snapshots().first().id!=before.id);
        QVERIFY(vault.exists());vault.lock();QVERIFY(portfolio.snapshots().isEmpty());
    }
    void validMutationsFollowBuildPolicy_data() {
        QTest::addColumn<RequestSpec>("request");
        const auto add = [](const char* name, Operation op, const QByteArray& method,
                            const QString& url, const QByteArray& body = {}) {
            RequestSpec r; r.operation = op; r.method = method; r.url = QUrl(url);
            r.body = body; r.authenticated = true; r.headers["X-MBX-APIKEY"] = "FAKE";
            QTest::newRow(name) << r;
        };
        const QByteArray common = "timestamp=100&recvWindow=5000&signature=fake";
        const QByteArray order = "symbol=BTCUSDT&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=100&newClientOrderId=fake&newOrderRespType=RESULT&" + common;
        add("spot-place", Operation::PlaceOrder, "POST", "https://api.binance.com/api/v3/order", order);
        add("spot-test", Operation::TestOrder, "POST", "https://api.binance.com/api/v3/order/test", order);
        add("spot-cancel", Operation::CancelOrder, "DELETE", "https://api.binance.com/api/v3/order?symbol=BTCUSDT&orderId=1&" + common);
        add("spot-amend", Operation::AmendOrder, "PUT", "https://api.binance.com/api/v3/order/amend/keepPriority",
            "symbol=BTCUSDT&orderId=1&newQty=1&newClientOrderId=fake&" + common);
        add("futures-place", Operation::PlaceOrder, "POST", "https://fapi.binance.com/fapi/v1/order",
            "symbol=BTCUSDT&side=BUY&type=LIMIT&positionSide=BOTH&quantity=1&price=100&timeInForce=GTC&newClientOrderId=fake&" + common);
        add("futures-algo-cancel", Operation::AlgoCancelOrder, "DELETE", "https://fapi.binance.com/fapi/v1/algoOrder?algoId=1&" + common);
        RequestSpec upbit; upbit.operation = Operation::PlaceOrder; upbit.method = "POST";
        upbit.authenticated = true; upbit.url = QUrl("https://api.upbit.com/v1/orders");
        upbit.headers["Authorization"] = "Bearer FAKE";
        upbit.body = R"({"market":"KRW-BTC","side":"bid","ord_type":"limit","volume":"1","price":"100","identifier":"fake"})";
        QTest::newRow("upbit-place") << upbit;
        auto cancel=upbit;cancel.operation=Operation::CancelOrder;cancel.method="DELETE";
        cancel.url=QUrl("https://api.upbit.com/v1/order?uuid=fake-order");cancel.body.clear();QTest::newRow("upbit-cancel")<<cancel;
        auto amend=upbit;amend.operation=Operation::AmendOrder;amend.url=QUrl("https://api.upbit.com/v1/orders/cancel_and_new");
        amend.body=R"({"prev_order_uuid":"fake-order","new_ord_type":"limit","new_volume":"1","new_price":"100","new_identifier":"new-fake"})";
        QTest::newRow("upbit-amend")<<amend;
        for(const auto op:{Operation::PlaceOrder,Operation::AmendOrder,Operation::CancelOrder}){
            const auto suffix=QString::number(int(op));
            RequestSpec broker;broker.operation=op;broker.method="POST";broker.authenticated=true;broker.body="{}";
            broker.url=QUrl("https://openapi.tossinvest.com/api/v1/orders"+(op==Operation::PlaceOrder?QString{}:op==Operation::AmendOrder?"/fake-order/modify":"/fake-order/cancel"));
            QTest::newRow(qPrintable("toss-"+suffix))<<broker;
            for(const auto& host:QStringList{"api.kiwoom.com","mockapi.kiwoom.com"}){
                broker.url=QUrl("https://"+host+"/api/dostk/ordr");broker.headers={{"api-id",op==Operation::PlaceOrder?"kt10000":op==Operation::AmendOrder?"kt10002":"kt10003"}};
                QTest::newRow(qPrintable(host+"-"+suffix))<<broker;
            }
            for(const auto& host:QStringList{"openapi.koreainvestment.com:9443","openapivts.koreainvestment.com:29443"}){
                broker.url=QUrl("https://"+host+"/uapi/domestic-stock/v1/trading/"+(op==Operation::PlaceOrder?"order-cash":"order-rvsecncl"));broker.headers.clear();
                QTest::newRow(qPrintable(host+"-"+suffix))<<broker;
            }
            broker.url=QUrl("https://openapi.ls-sec.co.kr:8080/stock/order");broker.headers={{"tr_cd",op==Operation::PlaceOrder?"CSPAT00601":op==Operation::AmendOrder?"CSPAT00701":"CSPAT00801"}};
            QTest::newRow(qPrintable("ls-"+suffix))<<broker;
            RequestSpec okx;okx.operation=op;okx.method="POST";okx.authenticated=true;
            okx.url=QUrl("https://openapi.okx.com/api/v5/trade/"+QString(op==Operation::PlaceOrder?"order":op==Operation::AmendOrder?"amend-order":"cancel-order"));
            for(const auto* header:{"OK-ACCESS-KEY","OK-ACCESS-SIGN","OK-ACCESS-TIMESTAMP","OK-ACCESS-PASSPHRASE"})okx.headers[header]="FAKE";
            okx.body=op==Operation::PlaceOrder?QByteArray(R"({"instId":"BTC-USDT","tdMode":"cash","side":"buy","ordType":"limit","sz":"1","px":"100","clOrdId":"fake"})"):
                op==Operation::AmendOrder?QByteArray(R"({"instId":"BTC-USDT","ordId":"1","newSz":"2","cxlOnFail":false})"):QByteArray(R"({"instId":"BTC-USDT","ordId":"1"})");
            QTest::newRow(qPrintable("okx-"+suffix))<<okx;
        }
        RequestSpec bithumb; bithumb.operation=Operation::PlaceOrder;bithumb.method="POST";bithumb.authenticated=true;
        bithumb.url=QUrl("https://api.bithumb.com/v2/orders");bithumb.headers["Authorization"]="Bearer FAKE";
        bithumb.body=R"({"market":"KRW-BTC","side":"bid","order_type":"limit","volume":"1","price":"100","client_order_id":"fake"})";
        QTest::newRow("bithumb-place")<<bithumb;
        bithumb.operation=Operation::CancelOrder;bithumb.method="DELETE";bithumb.url=QUrl("https://api.bithumb.com/v2/order?order_id=fake");bithumb.body.clear();
        QTest::newRow("bithumb-cancel")<<bithumb;

    }
    void validMutationsFollowBuildPolicy() {
        QFETCH(RequestSpec, request);
        // Live routes follow the build policy; retired broker hosts are rejected
        // in both builds, before either cooldown handling or network dispatch.
        const bool retiredHost = QStringList{"mockapi.kiwoom.com", "openapivts.koreainvestment.com"}.contains(request.url.host());
        QCOMPARE(NetworkTradingTransport::allowed(request), !readOnlyBuild() && !retiredHost);
        // No socket is opened: both the read-only gate and a shared cooldown
        // reject before dispatch. Exercise the real transport, not only its predicate.
        qint64 now=1000;auto limits=std::make_shared<TradingRequestLimits>([&]{return now;});
        HttpResult limited;limited.status=429;limited.headers["Retry-After"]="90";limits->observe(request.url,limited);
        NetworkTradingTransport transport(nullptr,limits);QSignalSpy completed(&transport,&TradingTransport::finished);
        transport.send(1,request);QTRY_COMPARE(completed.size(),1);
        const auto response=qvariant_cast<HttpResult>(completed.first().at(1));
        if(readOnlyBuild() || retiredHost){QVERIFY(response.networkError);QCOMPARE(response.localRetryAfterSeconds,0);}
        else {QCOMPARE(response.localRetryAfterSeconds,90);const auto rejected=localRateLimitFailure(response);
            QVERIFY(rejected.definiteRejection);QVERIFY(!rejected.unknown);QVERIFY(!rejected.needsReconciliation);}
        request.operation = Operation::Account;
        QVERIFY(!NetworkTradingTransport::allowed(request));
    }
    void cooldownIsSharedAcrossKeysAndTransports() {
        qint64 now=1000;auto limits=std::make_shared<TradingRequestLimits>([&]{return now;});
        const QUrl origin("https://api.binance.com");HttpResult response;response.status=429;response.headers["retry-after"]="90";
        limits->observe(origin,response);QCOMPARE(limits->remainingMs(QUrl("https://fapi.binance.com")),90000);
        QCOMPARE(limits->remainingMs(QUrl("https://testnet.binance.vision")),0);QCOMPARE(limits->remainingMs(QUrl("https://api.upbit.com")),0);
        NetworkTradingTransport a(nullptr,limits),b(nullptr,limits);
        QSignalSpy first(&a,&TradingTransport::finished),second(&b,&TradingTransport::finished);
        RequestSpec request;request.operation=Operation::Account;request.authenticated=true;
        request.url=QUrl("https://api.binance.com/api/v3/account?timestamp=100&recvWindow=5000&signature=fake");
        request.headers["X-MBX-APIKEY"]="FakeFirstKey";a.send(1,request);
        request.headers["X-MBX-APIKEY"]="FakeSecondKey";b.send(2,request);
        QTRY_COMPARE(first.size(),1);QTRY_COMPARE(second.size(),1);
        QCOMPARE(qvariant_cast<HttpResult>(first.first().at(1)).localRetryAfterSeconds,90);
        QCOMPARE(qvariant_cast<HttpResult>(second.first().at(1)).localRetryAfterSeconds,90);
        now+=30000;limits->observe(origin,qvariant_cast<HttpResult>(first.first().at(1)));
        HttpResult success;success.status=200;limits->observe(origin,success);QCOMPARE(limits->remainingMs(origin),60000);
        response.headers["retry-after"]="120";limits->observe(origin,response);QCOMPARE(limits->remainingMs(origin),120000);
        now+=120000;QCOMPARE(limits->remainingMs(origin),0);
    }
    void exhaustedPublicQuotaAndInvalidRetryAfterAreConservative() {
        qint64 now=1000;TradingRequestLimits limits([&]{return now;});HttpResult response;response.status=200;
        const QUrl upbit("https://api.upbit.com/v1/ticker?markets=KRW-BTC");
        response.headers["Remaining-Req"]="group=ticker; min=1800; sec=0";limits.observe(upbit,response);
        QCOMPARE(limits.remainingMs(QUrl("https://api.upbit.com/v1/accounts")),1100);
        now+=1100;QCOMPARE(limits.remainingMs(upbit),0);
        response={};response.status=418;response.headers["Retry-After"]="invalid";limits.observe(upbit,response);
        QCOMPARE(limits.remainingMs(upbit),60000);
        response.headers["Retry-After"]="-1";limits.observe(upbit,response);QCOMPARE(limits.remainingMs(upbit),60000);
        const QUrl kis("https://openapi.koreainvestment.com:9443");response.status=429;response.headers["Retry-After"]="70";
        limits.observe(kis,response);QCOMPARE(limits.remainingMs(kis),70000);
        QCOMPARE(limits.remainingMs(QUrl("https://openapivts.koreainvestment.com:29443")),0);
    }
    void readRoutesRemainBoundToTheirPayloads() {
        RequestSpec r; r.operation = Operation::Quote;
        r.url = QUrl("https://api.upbit.com/v1/ticker?markets=KRW-PUMP");
        QVERIFY(NetworkTradingTransport::allowed(r));
        r.operation = static_cast<Operation>(999);
        QVERIFY(!NetworkTradingTransport::allowed(r));
        QString error;
        auto rpc = SolanaReadRpc::request(SwapOperation::Balance, "fake-read",
            {QStringLiteral("11111111111111111111111111111111"), QJsonObject{{"commitment", "confirmed"}}}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(rpc.method, QByteArray("POST"));
        QVERIFY(NetworkSwapTransport::allowed(rpc));
        auto json = QJsonDocument::fromJson(rpc.body).object(); json["method"] = "sendTransaction";
        rpc.body = QJsonDocument(json).toJson();
        QVERIFY(!NetworkSwapTransport::allowed(rpc));
    }

    void exchangeRequestBuildersCannotConstructFinancialWrites() {
        for (const auto& provider : QStringList{"upbit", "binance", "okx", "bithumb", "binance-usdm"}) {
            test::ReadFixture fixture(provider);
            CredentialSet credentials;
            for (auto it=fixture.credentials.cbegin();it!=fixture.credentials.cend();++it)
                { const auto bytes=it.value().toUtf8(); credentials.emplace(it.key(), SecureBuffer(bytes.constData(),size_t(bytes.size()))); }
            std::unique_ptr<ExchangeAdapter> adapter;
            if (provider=="upbit") adapter=std::make_unique<UpbitAdapter>();
            else if (provider=="binance") adapter=std::make_unique<BinanceAdapter>();
            else if (provider=="okx") adapter=std::make_unique<OkxAdapter>(fixture.account);
            else if (provider=="bithumb") adapter=std::make_unique<BithumbAdapter>();
            else adapter=std::make_unique<BinanceUsdmAdapter>(fixture.account);
            OrderInput input; input.symbol=(provider=="upbit"||provider=="bithumb")?"KRW-BTC":provider=="okx"?"BTC-USDT":"BTCUSDT";
            input.side="buy"; input.type="limit"; input.quantity="1"; input.price="100"; input.clientId="synthetic";
            for (auto op : {Operation::PlaceOrder,Operation::CancelOrder,Operation::AmendOrder,Operation::TestOrder,Operation::AlgoCancelOrder}) {
                QString error;
                const auto request=adapter->build(op,fixture.account,input,credentials,QDateTime::currentMSecsSinceEpoch(),&error);
                QVERIFY2(request.url.isEmpty(),qPrintable(provider+" generated a financial request"));
                QVERIFY(!error.isEmpty());
            }
        }
    }
    void swapExecutionRoutesAreAbsent() {
        QString error;
        for (const auto op : {SwapOperation::Order, SwapOperation::Execute, SwapOperation::Simulate}) {
            const auto rpc = SolanaReadRpc::request(op, "fake-read-only", {}, &error);
            QVERIFY(rpc.url.isEmpty()); QVERIFY(!error.isEmpty());
        }
        SwapRequest request; request.operation = SwapOperation::Execute;
        request.url = QUrl("https://api.jup.ag/swap/v2/execute");
        request.headers = {{"Content-Type", "application/json"}, {"x-api-key", "FAKE"}};
        request.body = R"({"requestId":"synthetic","signedTransaction":"AA==","lastValidBlockHeight":"100"})";
        QVERIFY(!NetworkSwapTransport::allowed(request));
        request.operation = SwapOperation::Balance;
        QVERIFY(!NetworkSwapTransport::allowed(request));
    }
    void addressValidationPreservesReadOnlyCompatibility() {
        const auto address = QStringLiteral("11111111111111111111111111111111");
        QVERIFY(SolanaTransactionCodec::publicKeyValid(address));
        QCOMPARE(SolanaTransactionCodec::decodeBase58(address, 32), QByteArray(32, '\0'));
        QCOMPARE(SolanaTransactionCodec::encodeBase58(QByteArray(32, '\0')), address);
        QVERIFY(!SolanaTransactionCodec::publicKeyValid("not-a-public-key"));
    }
    void accountReadAndHomeEntry() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); Vault vault(dir.path() + "/vault.bin");
        QVERIFY(vault.create("Fake read-only home! 2026"));
        AccountRecord a; a.provider = "binance"; a.alias = "가짜 조회 계좌";
        a.environment = "live"; a.market = "spot"; a.currency = "USDT";
        QVERIFY(vault.addAccount(a, {{"api_key", "FakeKeyOnly123"}, {"secret", "FakeSecretOnly123"}}, StorageMode::EncryptedVault));
        auto* transport = new FakeAssetTransport;
        TradingController controller(vault, transport);
        QVERIFY(controller.selectAccount(vault.accounts().first().id));
        QVERIFY(controller.refreshAssets());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QVERIFY2(transport->sent >= 2, qPrintable(controller.status()));
        QCOMPARE(controller.balances().size(), 1);
        QCOMPARE(controller.balances().first().locked, QString("0.5"));
        AccountModel accounts(&vault); LocalWindow window(vault, accounts);
        window.setSecurityStatus(true, "FAKE_READY");
        auto* list = window.findChild<QListView*>("accountList"); QVERIFY(list);
        list->setCurrentIndex(accounts.index(0));
        QVERIFY(!window.findChild<QPushButton*>("openTrading"));
    }
};
QTEST_MAIN(ReadOnlyTest)
#include "tst_read_only.moc"
