#include "keefi/BinanceBalanceStream.h"
#include "keefi/Vault.h"
#include "keefi/TradingRequestLimits.h"
#include "fixtures/BinanceBalanceSocket.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
using namespace keefi;
namespace {
constexpr qint64 Now=1792400000000LL;
const QString Password="Fake Balance Password! 2026";
bool prepare(Vault& vault,QString environment="live") {
    AccountRecord a;a.provider="binance";a.market="spot";a.environment=environment;a.alias="fake balance";a.currency="USDT";
    return vault.create(Password)&&vault.addAccount(a,{{"api_key","FakeBalanceKey123"},{"secret","FakeBalanceSecret123"}},StorageMode::EncryptedVault);
}
}
class BinanceBalanceTests final : public QObject {
    Q_OBJECT
private slots:
    void signsOnlyReadOnlySubscriptionAfterPublicServerTime() {
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));
        auto wire=std::make_unique<test::BalanceSocket>();auto* s=wire.get();BinanceBalanceStream stream(v,std::move(wire));
        QSignalSpy updates(&stream,&BinanceBalanceStream::balancesChanged);
        stream.start(v.accounts().first().id);QCOMPARE(s->opened,QUrl("wss://ws-api.binance.com:443/ws-api/v3"));QVERIFY(s->sent.isEmpty());
        emit s->connected();QCOMPARE(s->sent.size(),1);QVERIFY(!s->sent.first().contains("FakeBalance"));
        QCOMPARE(QJsonDocument::fromJson(s->sent.first()).object().value("method").toString(),QString("time"));
        s->time(Now);QCOMPARE(s->sent.size(),2);const auto request=QJsonDocument::fromJson(s->sent.last()).object();
        QCOMPARE(request.value("method").toString(),QString("userDataStream.subscribe.signature"));
        const auto params=request.value("params").toObject();QCOMPARE(params.size(),4);
        QCOMPARE(params.value("apiKey").toString(),QString("FakeBalanceKey123"));QCOMPARE(params.value("timestamp").toInteger(),Now);
        QCOMPARE(params.value("recvWindow").toInt(),5000);
        QCOMPARE(params.value("signature").toString(),QString("063a75b85d68691ed150e1da8ac8d8cb72772572df7bcbe9a492304c5c55cc4b"));
        QVERIFY(!s->sent.last().contains("FakeBalanceSecret"));s->subscribed();QVERIFY(stream.ready());QCOMPARE(updates.size(),1);
        QVERIFY(!stream.status().contains("FakeBalance"));
    }
    void ignoresOtherSubscriptionsDuplicatesFutureAndUnknownEvents() {
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));
        auto wire=std::make_unique<test::BalanceSocket>();auto* s=wire.get();BinanceBalanceStream stream(v,std::move(wire));
        QSignalSpy updates(&stream,&BinanceBalanceStream::balancesChanged);stream.start(v.accounts().first().id);s->handshake(Now);updates.clear();
        s->event(Now,"outboundAccountPosition",8);s->event(Now,"unknown");s->event(Now+60000);QCOMPARE(updates.size(),0);
        s->event(Now);s->event(Now);s->event(Now-1);QCOMPARE(updates.size(),1);
        s->event(Now+1);s->event(Now,"balanceUpdate");s->event(Now,"executionReport");s->event(Now,"externalLockUpdate");QCOMPARE(updates.size(),5);
        // Only invalidations leave this object; quantities/order fields are never used as a snapshot.
        QCOMPARE(s->sent.size(),2);
    }
    void lockAndCredentialReplacementAbortBeforeSigning() {
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));const auto id=v.accounts().first().id;
        auto wire=std::make_unique<test::BalanceSocket>();auto* s=wire.get();BinanceBalanceStream stream(v,std::move(wire));
        stream.start(id);emit s->connected();const auto request=s->lastId();v.lock();
        s->deliver({{"id",request},{"status",200},{"result",QJsonObject{{"serverTime",Now}}}});QCOMPARE(s->sent.size(),1);QVERIFY(!stream.ready());
        QVERIFY(v.unlock(Password));stream.start(id);emit s->connected();
        QVERIFY(v.replaceCredentials(id,{{"api_key","FakeReplacementKey123"},{"secret","FakeReplacementSecret123"}}));
        s->time(Now);QCOMPARE(s->sent.size(),2);QVERIFY(!stream.ready());
    }
    void lockFromSubscribeStatusPreventsCredentialSend() {
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));
        auto wire=std::make_unique<test::BalanceSocket>();auto* s=wire.get();BinanceBalanceStream stream(v,std::move(wire));
        connect(&stream,&BinanceBalanceStream::changed,&v,[&]{if(stream.status().contains("구독 확인 중"))v.lock();});
        stream.start(v.accounts().first().id);emit s->connected();s->time(Now);QCOMPARE(s->sent.size(),1);QVERIFY(!v.isUnlocked());
    }
    void networkReconnectResignsAndInvalidatesTheGap() {
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));
        auto wire=std::make_unique<test::BalanceSocket>();auto* s=wire.get();BinanceBalanceStream stream(v,std::move(wire));
        QSignalSpy updates(&stream,&BinanceBalanceStream::balancesChanged);stream.start(v.accounts().first().id);s->handshake(Now);const auto signature=s->sent.last();
        emit s->failed();QVERIFY(!stream.ready());s->event(Now+1);QCOMPARE(updates.size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(s->opens,2,2000);s->handshake(Now+2000);QVERIFY(stream.ready());QCOMPARE(updates.size(),2);QVERIFY(signature!=s->sent.last());
        emit s->failed();stream.stop();QTest::qWait(1200);QCOMPARE(s->opens,2);QVERIFY(!stream.ready());
    }
    void authenticationFailureHasNoAutomaticReplayOrServerText() {
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));
        auto wire=std::make_unique<test::BalanceSocket>();auto* s=wire.get();BinanceBalanceStream stream(v,std::move(wire));
        stream.start(v.accounts().first().id);emit s->connected();s->time(Now);
        s->deliver({{"id",s->lastId()},{"status",401},{"error",QJsonObject{{"code",-2015},{"msg","FAKE_PRIVATE_ACCOUNT_SECRET"}}}});
        QTest::qWait(1200);QCOMPARE(s->opens,1);QCOMPARE(s->sent.size(),2);QVERIFY(!stream.ready());QVERIFY(!stream.status().contains("FAKE_PRIVATE"));
    }
    void rateLimitIsSharedWithRestAndBlocksManualReconnect() {
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));qint64 monotonic=0;
        auto limits=std::make_shared<TradingRequestLimits>([&]{return monotonic;});
        auto wire=std::make_unique<test::BalanceSocket>();auto* s=wire.get();BinanceBalanceStream stream(v,std::move(wire),limits);
        const auto id=v.accounts().first().id;stream.start(id);emit s->connected();s->time(Now);
        s->deliver({{"id",s->lastId()},{"status",429},{"error",QJsonObject{{"data",QJsonObject{{"retryAfter",Now+90000}}}}}});
        QVERIFY(limits->remainingMs(QUrl("https://api.binance.com/api/v3/account"))>=89000);
        QVERIFY(limits->remainingMs(QUrl("https://fapi.binance.com/fapi/v3/account"))>=89000);
        QCOMPARE(limits->remainingMs(QUrl("https://testnet.binance.vision/api/v3/account")),0);
        stream.start(id);QCOMPARE(s->opens,1);QVERIFY(stream.status().contains("요청 제한"));stream.stop();
        monotonic+=100000;stream.start(id);QCOMPARE(s->opens,2);
    }
    void retiredConnectionsCannotBeCreatedAndLockedConnectionsNeverAuthenticate() {
        QTemporaryDir dir;Vault retired(dir.filePath("retired"));QVERIFY(!prepare(retired,"testnet"));QVERIFY(retired.accounts().isEmpty());
        Vault v(dir.filePath("live"));QVERIFY(prepare(v));const auto id=v.accounts().first().id;
        auto wire=std::make_unique<test::BalanceSocket>();auto* s=wire.get();BinanceBalanceStream stream(v,std::move(wire));
        stream.start(id);QCOMPARE(s->opens,1);stream.stop();emit s->connected();QVERIFY(s->sent.isEmpty());
        v.lock();stream.start(id);QCOMPARE(s->opens,1);emit s->connected();QVERIFY(s->sent.isEmpty());
    }
    void malformedAndTerminatedStreamsFallBackWithoutRawData() {
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));
        auto wire=std::make_unique<test::BalanceSocket>();auto* s=wire.get();BinanceBalanceStream stream(v,std::move(wire));
        const auto id=v.accounts().first().id;stream.start(id);s->handshake(Now);
        emit s->received("{\"event\":{},\"event\":\"FAKE_PRIVATE\"}");QVERIFY(!stream.ready());QVERIFY(!stream.status().contains("FAKE_PRIVATE"));
        stream.start(id);s->handshake(Now);s->event(Now,"eventStreamTerminated");QVERIFY(!stream.ready());
        stream.start(id);s->handshake(Now);emit s->received(QByteArray(256*1024+1,'x'));QVERIFY(!stream.ready());
    }
};
QTEST_GUILESS_MAIN(BinanceBalanceTests)
#include "tst_binance_balance_stream.moc"
