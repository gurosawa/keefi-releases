#include "keefi/MarketStudies.h"
#include <QTest>
#include <QFile>
#include <QJsonDocument>
using namespace keefi;
namespace {
constexpr qint64 Start=1792598400000;
QList<MarketCandle> flat(int count) {
    QList<MarketCandle> rows;
    for(int i=0;i<count;++i)rows.append({Start+i*MarketCandles::IntervalMs,Start+i*MarketCandles::IntervalMs+1000,"100","110","90","100","2"});
    return rows;
}
}
class MarketStudiesTest:public QObject {
    Q_OBJECT
  private slots:
    void selectedTimeframeControlsGapsAndHigherGroups(){
        for(const qint64 interval:{60000LL,300000LL,900000LL,3600000LL,14400000LL,86400000LL}){
            auto rows=flat(100);
            for(int i=0;i<rows.size();++i){rows[i].startsAtMs=Start/interval*interval+i*interval;rows[i].lastStoredTickMs=rows[i].startsAtMs+1000;}
            const auto r=candleStudies(rows,interval);QVERIFY2(r.available,qPrintable(QString::number(interval)));
            QCOMPARE(r.trends[0].minutes,int(interval/60000));QVERIFY(r.trends[0].available);
            if(interval>=3600000)QVERIFY(r.sessions.isEmpty());
            rows[90].startsAtMs+=interval;QVERIFY(!candleStudies(rows,interval).available);
        }
    }
    void nonconstantFixtureMatchesIndependentArithmetic(){
        auto rows=flat(60);for(int i=0;i<60;++i){const auto c=QString::number(100+(i*7)%13+i);const auto close=decimal::multiply(c,"0.1");
            rows[i].open=rows[i].close=close;rows[i].high=decimal::add(close,"2");rows[i].low=decimal::subtract(close,"2");rows[i].volume=QString::number(2+(i*17)%7);}
        const auto r=candleStudies(rows);QVERIFY(r.available);QVERIFY(r.adxAvailable);QVERIFY(r.profileAvailable);
        QVERIFY(qAbs(r.adx-16.547103835343033)<1e-10);QCOMPARE(r.profileVolume,299.0);
        QVERIFY(qAbs(r.profileLow-12.905)<1e-10);QVERIFY(qAbs(r.trends[0].mean-15.54)<1e-10);
        if(const auto path=qEnvironmentVariable("KEEFI_G8_STUDIES_RESULT");!path.isEmpty()){
            const QJsonObject object{{"adx",r.adx},{"plusDi",r.plusDi},{"minusDi",r.minusDi},{"profileVolume",r.profileVolume},
                {"profileLow",r.profileLow},{"profileHigh",r.profileHigh},{"profileShare",r.profileShare},{"mean15",r.trends[0].mean}};
            QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(object).toJson());
        }
    }
    void sessionSamplesAndPastSimilarityNeverInventFutureOutcomes(){
        auto rows=flat(160);const int prices[]{100,103,101,106,102};
        for(int i=0;i<rows.size();++i)rows[i].open=rows[i].close=QString::number(prices[i%5]);
        const auto r=candleStudies(rows);QVERIFY(r.similarityAvailable);
        QCOMPARE(r.similaritySamples,121);QVERIFY(qAbs(r.similarity-1)<1e-10);
        QVERIFY(r.similarToMs<=rows[140].startsAtMs);
        int count=0;for(const auto& s:r.sessions){count+=s.bars;QCOMPARE(s.meanPercent,0.0);QCOMPARE(s.meanAbsolutePercent,0.0);}
        QCOMPARE(count,160);QVERIFY(!candleStudies(flat(100)).similarityAvailable);
        rows[80].volume="NaN";QVERIFY(!candleStudies(rows).available);
    }
    void hourlyTrendUsesOnlyCompleteClockAlignedGroups(){
        auto rows=flat(100);for(int i=0;i<100;++i){rows[i].open=rows[i].close=QString::number(100+i);
            rows[i].high=QString::number(101+i);rows[i].low=QString::number(99+i);}
        auto r=candleStudies(rows);QCOMPARE(r.trends.size(),2);
        QCOMPARE(r.trends[0].minutes,15);QVERIFY(r.trends[0].available);QCOMPARE(r.trends[0].direction,1);
        QCOMPARE(r.trends[1].minutes,60);QCOMPARE(r.trends[1].bars,25);QVERIFY(r.trends[1].available);
        QCOMPARE(r.trends[1].endsAtMs,Start+100*MarketCandles::IntervalMs);
        // 4h cannot warm up from 200 x 15m; it is not offered as an endless waiting state.
        rows.append({Start+100*MarketCandles::IntervalMs,0,"200","201","199","200","2"});
        const auto after=candleStudies(rows);QCOMPARE(after.trends[1].bars,25);QCOMPARE(after.trends[1].mean,r.trends[1].mean);
    }
    void representativeVolumeProfileIsExplicitAndKeepsVolume(){
        auto rows=flat(40);auto r=candleStudies(rows);
        QVERIFY(r.profileAvailable);QCOMPARE(r.profileVolume,80.0);QCOMPARE(r.profileBars,40);
        QVERIFY(r.profileLow<=100&&r.profileHigh>100);QCOMPARE(r.profileShare,100.0);
        QVERIFY(r.nearbyLevels.contains(QStringLiteral("이평20")));
        for(auto& row:rows)row.volume="0";
        QVERIFY(!candleStudies(rows).profileAvailable);
        rows=flat(21);rows.last().volume="100";rows.last().high="101";rows.last().low="99";
        QVERIFY(!candleStudies(rows).narrowHighVolume); // Previous 20 true ranges require a preceding close.
        rows=flat(22);rows.last().volume="100";rows.last().high="101";rows.last().low="99";
        QVERIFY(candleStudies(rows).narrowHighVolume);
        rows.last().volume="2";QVERIFY(!candleStudies(rows).narrowHighVolume);
    }
    void wilderAdxSeparatesDirectionFromStrength(){
        auto rows=flat(40);
        auto r=candleStudies(rows);QVERIFY(r.adxAvailable);QCOMPARE(r.adx,0.0);
        QCOMPARE(r.regime,QStringLiteral("방향성 약함"));
        for(int i=0;i<40;++i){rows[i].open=rows[i].close=QString::number(100+i);
            rows[i].high=QString::number(101+i);rows[i].low=QString::number(99+i);}
        r=candleStudies(rows);QVERIFY(r.adxAvailable);QVERIFY(qAbs(r.adx-100)<1e-10);
        QVERIFY(r.plusDi>0);QCOMPARE(r.minusDi,0.0);QCOMPARE(r.regime,QStringLiteral("방향성 강함"));
        QVERIFY(!candleStudies(rows.mid(0,27)).adxAvailable);
    }
    void sweepRequiresClosedReturnInsidePreviousTwentyBars(){
        auto rows=flat(21);rows.last().high="115";rows.last().close="105";
        const auto result=candleStudies(rows);
        QVERIFY(result.available);QVERIFY(result.sweptHigh);QVERIFY(!result.sweptLow);
        QCOMPARE(result.previousHigh,110.0);QCOMPARE(result.previousLow,90.0);
        rows.last().close="112";QVERIFY(!candleStudies(rows).sweptHigh);
        rows.last().low="85";rows.last().close="95";QVERIFY(candleStudies(rows).sweptLow);
        rows.last().startsAtMs+=MarketCandles::IntervalMs;
        QVERIFY(!candleStudies(rows).available);
    }
};
QTEST_GUILESS_MAIN(MarketStudiesTest)
#include "tst_market_studies.moc"
