#include "keefi/RankingStudies.h"
#include <QtTest>
using namespace keefi;
namespace {
ChartStudyObservation bars(){ChartStudyObservation o;o.series.available=true;o.series.intervalMs=3600000;o.series.identity="coin:upbit:KRW-SYN";o.series.sourceUrl="https://api.upbit.com/v1/candles/minutes/60";
 for(int i=0;i<30;++i)o.series.candles.append({(100+i)*3600000LL,0,"100","110","90","100",i==29?"30":"10"});return o;}
}
class RankingStudiesTest:public QObject {
 Q_OBJECT
private slots:
 void completedVolumesExcludeFormingAndCurrentFromBaseline(){auto o=bars();o.series.forming=MarketCandle{130*3600000LL,0,"1","1000","1","1000","90000"};const auto v=rankingCandleStudy(RankingStudy::Volume,o);QVERIFY(v.available);QCOMPARE(v.text,QStringLiteral("3.00배"));QCOMPARE(v.throughMs,130*3600000LL);}
 void drawdownUsesExplicitTwentyClosedBars(){auto o=bars();o.series.candles[0].high="1000";const auto v=rankingCandleStudy(RankingStudy::Drawdown,o);QVERIFY(v.available);QCOMPARE(v.text,QStringLiteral("▼9.09%"));QVERIFY(v.information.contains(QStringLiteral("20")));}
 void missingAndStaleAreNotZero(){auto o=bars();o.delayed=true;auto v=rankingCandleStudy(RankingStudy::Volume,o);QVERIFY(!v.available);QCOMPARE(v.text,QStringLiteral("지연"));o.delayed=false;o.series.candles.removeAt(20);v=rankingCandleStudy(RankingStudy::Volume,o);QVERIFY(!v.available);QVERIFY(!v.text.isEmpty());}
 void stockNeedsEligibleComparableTimeline(){auto o=bars();o.stock=true;auto v=rankingCandleStudy(RankingStudy::Drawdown,o);QVERIFY(!v.available);for(const auto& c:o.series.candles){ChartStudyTimelineRow row;row.startsAtMs=c.startsAtMs;row.completesAtMs=c.startsAtMs+3600000;row.eligible=true;row.connectsPrevious=true;o.timeline.rows.append(row);}o.timeline.adjustedComparable=true;v=rankingCandleStudy(RankingStudy::Drawdown,o);QVERIFY(v.available);o.timeline.rows.last().eligible=false;o.timeline.rows.last().reason=QStringLiteral("VI 이력 없음");v=rankingCandleStudy(RankingStudy::Drawdown,o);QVERIFY(!v.available);QCOMPARE(v.text,QStringLiteral("기준 확인 필요"));QCOMPARE(v.information,QStringLiteral("VI 이력 없음"));}
 void zeroBaselineAndInvalidBarsAreUnavailable(){auto o=bars();for(auto& c:o.series.candles)c.volume="0";QVERIFY(!rankingCandleStudy(RankingStudy::Volume,o).available);o=bars();o.series.candles.last().high="nan";QVERIFY(!rankingCandleStudy(RankingStudy::Drawdown,o).available);}
};
QTEST_GUILESS_MAIN(RankingStudiesTest)
#include "tst_ranking_studies.moc"
