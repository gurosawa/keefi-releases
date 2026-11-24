#include "keefi/ChartAlertEngine.h"
#include "keefi/SystemNotificationSink.h"
#include <QtTest>
using namespace keefi;
namespace {
class Sink final:public SystemNotificationSink {
public:
    QList<SystemNotice> posted;SystemNoticePermission allowed=SystemNoticePermission::Granted;
    SystemNoticePermission permission()const override{return allowed;}
    QString status()const override{return {};}
    void refreshPermission()override{}
    void requestAuthorization()override{}
    void post(const SystemNotice& n)override{posted.append(n);emit submissionFinished(n.identifier,n.privateEpoch,true);}
    void remove(const QStringList&)override{}
    void setPrivateContext(quint64,bool)override{}
    void stop()override{}
};
ChartPublicStudyEvent observationEvent(qint64 now,QString id="one"){
    return {{"coin","upbit","KRW-SYN"},"rapid_move",id,QStringLiteral("5분 ▲3.0%"),now,now,MovementDirection::Up,true};
}
}
class ChartPublicObservationsTest final:public QObject{
    Q_OBJECT
private slots:
    void optInAndLiveOnlyDedupe(){
        Sink sink;qint64 now=1728000000000;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
        engine.observePublicStudy(observationEvent(now));QVERIFY(sink.posted.isEmpty());
        QVERIFY(engine.setObservationEnabled("rapid_move",true));QVERIFY(sink.posted.isEmpty());
        now+=60000;auto historical=observationEvent(now);historical.liveEligible=false;engine.observePublicStudy(historical);QVERIFY(sink.posted.isEmpty());
        engine.observePublicStudy(observationEvent(now));engine.observePublicStudy(observationEvent(now));QCOMPARE(sink.posted.size(),1);
        QVERIFY(!sink.posted.first().privateData);QCOMPARE(sink.posted.first().privateEpoch,quint64(0));
        QVERIFY(sink.posted.first().title.contains(QStringLiteral("급변")));
        now+=60000;engine.observePublicStudy(observationEvent(now,"two"));QCOMPARE(sink.posted.size(),1);
    }
    void silenceDoesNotCatchUpAndDailyCapacityIsShared(){
        Sink sink;qint64 now=1728000000000;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
        QVERIFY(engine.setObservationEnabled("rsi",true));auto settings=engine.settings();settings.dailyLimit=1;engine.setSettings(settings);
        now+=60000;auto a=observationEvent(now);a.kind="rsi";sink.allowed=SystemNoticePermission::Denied;
        engine.observePublicStudy(a);sink.allowed=SystemNoticePermission::Granted;engine.observePublicStudy(a);QVERIFY(sink.posted.isEmpty());
        now+=60000;a=observationEvent(now,"next");a.kind="rsi";engine.observePublicStudy(a);QCOMPARE(sink.posted.size(),1);
        QVERIFY(engine.setObservationEnabled("macd",true));now+=60000;a=observationEvent(now,"other");a.kind="macd";engine.observePublicStudy(a);QCOMPARE(sink.posted.size(),1);
    }
    void liquidationSoundRequiresExplicitOptIn(){
        Sink sink;qint64 now=1728000000000;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
        auto value=observationEvent(now);value.kind="liquidation_feed";engine.observePublicStudy(value);QVERIFY(sink.posted.isEmpty());
        engine.setObservationEnabled("liquidation_feed",true);now+=60000;value=observationEvent(now);value.kind="liquidation_feed";engine.observePublicStudy(value);QCOMPARE(sink.posted.size(),1);QVERIFY(sink.posted.first().playSound);QVERIFY(!sink.posted.first().fixedRisk);
        engine.setObservationEnabled("liquidation_feed",false);now+=60000;value=observationEvent(now,"next");value.kind="liquidation_feed";engine.observePublicStudy(value);QCOMPARE(sink.posted.size(),1);
        engine.setObservationEnabled("rsi",true);now+=60000;value=observationEvent(now,"rsi");value.kind="rsi";engine.observePublicStudy(value);QCOMPARE(sink.posted.size(),2);QVERIFY(!sink.posted.last().playSound);
    }
    void publicSettingsAreSeparateAndAtomic(){
        Sink sink;qint64 now=1728000000000;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
        const auto listState=engine.publicState();QVERIFY(engine.setObservationEnabled("breakout",true));
        QCOMPARE(engine.publicState(),listState);QVERIFY(!engine.setObservationEnabled("place_order",true));
        ChartAlertEngine restored(&sink,nullptr,[&]{return now;});QVERIFY(restored.restoreObservationSettingsState(engine.observationSettingsState()));
        QVERIFY(restored.observationEnabled("breakout"));QVERIFY(sink.posted.isEmpty());
        const auto before=restored.observationSettingsState();QVERIFY(!restored.restoreObservationSettingsState("{\"account\":\"private\"}"));
        QCOMPARE(restored.observationSettingsState(),before);
    }
};
QTEST_GUILESS_MAIN(ChartPublicObservationsTest)
#include "tst_chart_public_observations.moc"
