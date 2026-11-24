#include "keefi/OkxOrderEvents.h"
#include "keefi/OkxProtocol.h"
#include "fixtures/OkxResponses.h"
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>

namespace keefi {
namespace {
constexpr qint64 Now = 1792200000000LL;
const QString Uid = "123456789";
CredentialSet fakeCredentials() {
    CredentialSet result;
    result.emplace("api_key", SecureBuffer::fromUtf8("FAKE_KEY_ONLY"));
    result.emplace("secret", SecureBuffer::fromUtf8("FAKE_SECRET_ONLY"));
    result.emplace("passphrase", SecureBuffer::fromUtf8("FAKE_PASSPHRASE_ONLY"));
    return result;
}
class FakeSocket final : public OkxEventSocket {
public:
    QUrl opened; QStringList sent; int opens = 0, aborts = 0;
    void open(const QUrl& url) override { opened = url; ++opens; }
    void sendText(const QString& text) override { sent.append(text); }
    void abort() override { ++aborts; }
    void deliver(QJsonObject value) { emit textReceived(QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact))); }
};
QJsonObject push(QJsonObject order, QString uid = Uid) {
    return {{"arg",QJsonObject{{"channel","orders"},{"instType","SPOT"},{"uid",uid}}},{"data",QJsonArray{order}}};
}
bool handshake(NetworkOkxOrderEvents& receiver, FakeSocket& socket) {
    emit socket.connected();
    socket.deliver({{"event","login"},{"code","0"}});
    if (socket.sent.size() != 2) return false;
    const auto subscribe = QJsonDocument::fromJson(socket.sent.last().toUtf8()).object();
    socket.deliver({{"event","subscribe"},{"id",subscribe.value("id")},
        {"arg",QJsonObject{{"channel","orders"},{"instType","SPOT"}}}});
    return receiver.ready();
}
}
class OkxEventTests final : public QObject {
    Q_OBJECT
private slots:
    void scopeSeparatesRegionAndRawUidAndRejectsRetiredEnvironment() {
        const auto scope = okxAccountScope("global","live",Uid);
        QCOMPARE(scope.size(),64);
        QVERIFY(scope != okxAccountScope("us","live",Uid));
        QVERIFY(okxAccountScope("global","demo",Uid).isEmpty());
        QVERIFY(scope != okxAccountScope("global","live","0" + Uid));
        QVERIFY(okxAccountScope("", "live",Uid).isEmpty());
        QVERIFY(okxAccountScope("global","live",Uid + "\n").isEmpty());
    }
    void fixedEndpointsAndReadOnlyLogin_data() {
        QTest::addColumn<QString>("region"); QTest::addColumn<QString>("environment"); QTest::addColumn<QString>("host");
        QTest::newRow("global-live") << QString("global") << QString("live") << QString("ws.okx.com");
        QTest::newRow("us-live") << QString("us") << QString("live") << QString("wsus.okx.com");
        QTest::newRow("eea-live") << QString("eea") << QString("live") << QString("wseea.okx.com");
    }
    void fixedEndpointsAndReadOnlyLogin() {
        QFETCH(QString,region); QFETCH(QString,environment); QFETCH(QString,host);
        auto wire = std::make_unique<FakeSocket>(); auto* socket = wire.get();
        NetworkOkxOrderEvents receiver(std::move(wire)); auto secrets = fakeCredentials();
        receiver.start(test::okxAccount(environment,region),secrets,okxAccountScope(region,environment,Uid),Now);
        secrets.clear(); // The start borrow ends synchronously; no key pointers survive.
        QCOMPARE(socket->opened,QUrl("wss://" + host + "/ws/v5/private"));
        QVERIFY(handshake(receiver,*socket));
        const auto login = QJsonDocument::fromJson(socket->sent[0].toUtf8()).object();
        QCOMPARE(login.value("op").toString(),QString("login"));
        const auto arg = login.value("args").toArray().first().toObject();
        QCOMPARE(arg.value("timestamp").toString(),QString("1792200000"));
        QCOMPARE(arg.value("sign").toString(),QString("igmKTqq/rnjao/tDJKk0FINmb0YF7odup4Abtj8aY94="));
        const auto subscription = QJsonDocument::fromJson(socket->sent[1].toUtf8()).object();
        QCOMPARE(subscription.value("op").toString(),QString("subscribe"));
        QCOMPARE(subscription.value("args").toArray(),QJsonArray({QJsonObject{{"channel","orders"},{"instType","SPOT"}}}));
        QVERIFY(!receiver.status().contains("FAKE_"));
        receiver.stop(); QVERIFY(!receiver.ready()); QCOMPARE(socket->sent.size(),2);
    }
    void invalidRegionNeverOpensSocket() {
        auto wire = std::make_unique<FakeSocket>(); auto* socket = wire.get();
        NetworkOkxOrderEvents receiver(std::move(wire)); auto secrets = fakeCredentials();
        receiver.start(test::okxAccount("live",""),secrets,okxAccountScope("global","live",Uid),Now);
        QCOMPARE(socket->opens,0); QVERIFY(!receiver.ready()); QVERIFY(socket->sent.isEmpty());
    }
    void retiredEnvironmentNeverOpensSocket() {
        auto wire = std::make_unique<FakeSocket>(); auto* socket = wire.get();
        NetworkOkxOrderEvents receiver(std::move(wire)); auto secrets = fakeCredentials();
        receiver.start(test::okxAccount("demo"),secrets,okxAccountScope("global","live",Uid),Now);
        QCOMPARE(socket->opens,0); QVERIFY(!receiver.ready()); QVERIFY(socket->sent.isEmpty());
    }
    void quoteAndBaseUnitsAreNeverSubtractedAcrossCurrencies() {
        OrderRecord r;
        auto order = test::okxOrder("100","partially_filled","buy","market","200","0.5","",Now);
        QVERIFY(parseOkxOrder(order,OkxOrderSource::Rest,&r));
        QCOMPARE(r.quantityUnit,QString("quote")); QCOMPARE(r.requestedQuoteAmount,QString("200"));
        QCOMPARE(r.executedBaseAmount,QString("0.5")); QVERIFY(r.quantity.isEmpty()); QVERIFY(r.remaining.isEmpty());
        order["side"] = "sell"; // An external quote-sized sell must retain its explicit unit.
        QVERIFY(parseOkxOrder(order,OkxOrderSource::Rest,&r)); QVERIFY(r.cancelable); QCOMPARE(r.quantityUnit,QString("quote"));
        order["tgtCcy"] = "base_ccy"; order["sz"] = "2";
        QVERIFY(parseOkxOrder(order,OkxOrderSource::Rest,&r)); QCOMPARE(r.remainingBaseAmount,QString("1.5"));
        order["tdMode"] = "cross"; QVERIFY(parseOkxOrder(order,OkxOrderSource::Rest,&r)); QVERIFY(!r.cancelable);
        order["tdMode"] = "cash"; order["attachAlgoOrds"] = QJsonArray{QJsonObject{{"algoId","9"}}};
        QVERIFY(parseOkxOrder(order,OkxOrderSource::Rest,&r)); QVERIFY(!r.cancelable);
        order = test::okxOrder(); order["reqId"] = "fakeamend1"; order["amendResult"] = "0";
        QVERIFY(parseOkxOrder(order,OkxOrderSource::Rest,&r)); QVERIFY(r.amendRequestId.isEmpty());
        QVERIFY(parseOkxOrder(order,OkxOrderSource::WebSocket,&r)); QCOMPARE(r.amendRequestId,QString("fakeamend1"));
    }
    void duplicateOutOfOrderAndLateEventsAreDiscarded() {
        auto wire = std::make_unique<FakeSocket>(); auto* socket = wire.get();
        NetworkOkxOrderEvents receiver(std::move(wire)); auto secrets = fakeCredentials();
        const auto scope = okxAccountScope("global","live",Uid);
        receiver.start(test::okxAccount(),secrets,scope,Now); QVERIFY(handshake(receiver,*socket));
        QSignalSpy spy(&receiver,&OkxOrderEvents::orderEvent);
        auto row = test::okxOrder("100","live","buy","limit","2","0","100",Now);
        row["reqId"] = "amend001"; row["amendResult"] = "0";
        socket->deliver(push(row)); QCOMPARE(spy.size(),1);
        const auto first = qvariant_cast<OkxOrderEvent>(spy[0][0]);
        QCOMPARE(first.accountScope,scope); QCOMPARE(first.generation,receiver.generation());
        socket->deliver(push(row)); QCOMPARE(spy.size(),1);
        row["uTime"] = QString::number(Now-1); row["reqId"] = ""; row["amendResult"] = "";
        socket->deliver(push(row)); QCOMPARE(spy.size(),1);
        receiver.stop(); const auto stopped = receiver.generation();
        row["uTime"] = QString::number(Now+1); socket->deliver(push(row)); emit socket->connected();
        QCOMPARE(spy.size(),1); QCOMPARE(receiver.generation(),stopped); QCOMPARE(socket->sent.size(),2);
    }
    void wrongAccountAndConflictingAmendResultsFailClosed() {
        for (const bool wrongAccount : {true,false}) {
            auto wire = std::make_unique<FakeSocket>(); auto* socket = wire.get();
            NetworkOkxOrderEvents receiver(std::move(wire)); auto secrets = fakeCredentials();
            receiver.start(test::okxAccount(),secrets,okxAccountScope("global","live",Uid),Now);
            QVERIFY(handshake(receiver,*socket)); QSignalSpy spy(&receiver,&OkxOrderEvents::orderEvent);
            auto row = test::okxOrder("100","live","buy","limit","2","0","100",Now);
            row["reqId"] = "sameRequest"; row["amendResult"] = "0";
            if (wrongAccount) socket->deliver(push(row,"999999"));
            else { socket->deliver(push(row)); row["amendResult"] = "-1"; socket->deliver(push(row)); }
            QVERIFY(!receiver.ready()); QCOMPARE(spy.size(),wrongAccount ? 0 : 1);
            QCOMPARE(socket->sent.size(),2); QVERIFY(!receiver.status().contains(Uid));
        }
    }
    void lockInEventSlotStopsRemainingBatch() {
        auto wire = std::make_unique<FakeSocket>(); auto* socket = wire.get();
        NetworkOkxOrderEvents receiver(std::move(wire)); auto secrets = fakeCredentials();
        receiver.start(test::okxAccount(),secrets,okxAccountScope("global","live",Uid),Now); QVERIFY(handshake(receiver,*socket));
        QSignalSpy spy(&receiver,&OkxOrderEvents::orderEvent);
        connect(&receiver,&OkxOrderEvents::orderEvent,&receiver,[&receiver] { receiver.stop(); });
        auto batch = push(test::okxOrder()); batch["data"] = QJsonArray{test::okxOrder("100"),test::okxOrder("101")};
        socket->deliver(batch); QCOMPARE(spy.size(),1); QVERIFY(!receiver.ready()); QCOMPARE(socket->sent.size(),2);
    }
};
} // namespace keefi
QTEST_GUILESS_MAIN(keefi::OkxEventTests)
#include "tst_okx_events.moc"
