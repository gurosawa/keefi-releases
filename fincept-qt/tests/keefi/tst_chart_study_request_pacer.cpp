#include "keefi/ChartStudyRequestPacer.h"
#include "keefi/ChartPublicContextSource.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/TradingTransport.h"
#include <QTest>
using namespace keefi;
namespace {
const QUrl Upbit("https://api.upbit.com/v1/candles/days"),Binance("https://api.binance.com/api/v3/klines"),Future("https://fapi.binance.com/fapi/v1/klines");
class FakeTransport final:public TradingTransport {public:QList<RequestSpec> sent;quint64 token=0;void send(quint64 t,const RequestSpec&r)override{token=t;sent.append(r);}void abort()override{};void answer(){HttpResult r;r.status=503;emit finished(token,r);}};
}
class ChartStudyRequestPacerTest:public QObject {
 Q_OBJECT
 private slots:
 void spacingAndFifoFairness(){qint64 now=10000;ChartStudyRequestPacer p([&]{return now;});auto a=p.createClient(),b=p.createClient(),c=p.createClient();QVERIFY(p.tryAcquire(a,Upbit));QVERIFY(!p.tryAcquire(b,Upbit));QVERIFY(!p.tryAcquire(c,Upbit));now+=999;QVERIFY(!p.tryAcquire(b,Upbit));++now;QVERIFY(!p.tryAcquire(a,Upbit));QVERIFY(p.tryAcquire(b,Upbit));now+=1000;QVERIFY(!p.tryAcquire(b,Upbit));QVERIFY(p.tryAcquire(c,Upbit));now+=1000;QVERIFY(p.tryAcquire(a,Upbit));}
 void originsAndCancellation(){qint64 now=10000;ChartStudyRequestPacer p([&]{return now;});auto a=p.createClient(),b=p.createClient();QVERIFY(p.tryAcquire(a,Upbit));QVERIFY(p.tryAcquire(b,Binance));QVERIFY(!p.tryAcquire(a,Future));QVERIFY(!p.tryAcquire(b,Upbit));p.cancel(b);now+=1000;QVERIFY(p.tryAcquire(a,Upbit));p.release(a);QVERIFY(!p.tryAcquire(a,Upbit));QVERIFY(!p.tryAcquire(b,Upbit));now+=1000;QVERIFY(p.tryAcquire(b,Upbit));}
 void twoRealSourcesSharePacerWithoutDequeuing(){
  qint64 now=1793241000000;auto pacer=std::make_shared<ChartStudyRequestPacer>([&]{return now;});FakeTransport extra,levels;
  ChartStudyFeed feed(&extra,nullptr,nullptr,[&]{return now;});ChartPublicContextSource context(&levels,nullptr,[&]{return now;});feed.setRequestPacer(pacer);context.setRequestPacer(pacer);
  context.setSelection("coin:upbit:KRW-BTC");context.setDemand(true,false);QCOMPARE(levels.sent.size(),1);feed.setDemands("test",{{{"coin","upbit","KRW-BTC"},3600000,1}});feed.pump();QVERIFY(extra.sent.isEmpty());levels.answer();
  now+=1000;context.pump();QCOMPARE(levels.sent.size(),1);feed.pump();QCOMPARE(extra.sent.size(),1);extra.answer();now+=1000;context.pump();QCOMPARE(levels.sent.size(),2);QVERIFY(levels.sent.last().url.path().endsWith("weeks"));
 }
};
QTEST_GUILESS_MAIN(ChartStudyRequestPacerTest)
#include "tst_chart_study_request_pacer.moc"
