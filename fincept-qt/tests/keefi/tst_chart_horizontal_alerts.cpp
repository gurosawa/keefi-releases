// C8 test-first assertions. All account/source values are synthetic.
#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartDrawings.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/StockChartSource.h"
#include "keefi/StockChartTypes.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QDateTime>
#include <QTimeZone>
using namespace keefi;
namespace {
constexpr qint64 Base=1728000000000,Minute=60000;
const WatchlistIdentity Coin{"coin","upbit","KRW-SYN"};
class Sink final:public SystemNotificationSink {
  public:
    QList<SystemNotice> posted;QStringList removed;bool autoAck=true;
    SystemNoticePermission permission()const override{return SystemNoticePermission::Granted;}
    QString status()const override{return "synthetic";}
    void refreshPermission()override{} void requestAuthorization()override{QFAIL("Engine must not request permission while observing a line");}
    void post(const SystemNotice& n)override{posted.append(n);if(autoAck)emit submissionFinished(n.identifier,n.privateEpoch,true);}
    void remove(const QStringList& ids)override{removed.append(ids);}
    void setPrivateContext(quint64,bool)override{} void stop()override{}
};
ChartHorizontalLineRule line(){return {"11111111-1111-4111-8111-111111111111","public-lines",QString(64,'a'),Coin,"KRW","100",Minute,true};}
MarketCandleSeries series(int count,qint64 now){
    MarketCandleSeries out;out.available=true;out.identity=Coin.key();out.asset="SYN";out.currency="KRW";out.intervalMs=Minute;out.receivedAtMs=now;out.sourceUrl=MarketCandles::request(out.identity,Minute).url.toString();
    for(int i=0;i<count;++i)out.candles.append({Base+i*Minute,0,"99","99.5","98","99","1"});return out;
}
void touch(MarketCandle& bar){bar.open="99";bar.high="101";bar.low="98";bar.close="100";}
bool configure(ChartAlertEngine& engine){
    if(!engine.setPublicLists({{"public-lines",{Coin}}}))return false;auto rule=engine.rule("public-lines");rule.enabled=true;rule.pullback=rule.results=rule.whales=false;rule.horizontal=true;rule.cooldownMs=0;
    return engine.setRule(rule)&&engine.setHorizontalLines({line()});
}
}
class HorizontalAlertDraft final:public QObject {
    Q_OBJECT
  private slots:
    void twoActualClosedBarsWorkWithoutStudyWarmup();
    void inclusiveTouchEpisodeRearmsOnlyAfterActualNonTouch();
    void gapJumpAndFormingCannotBecomeObservedTouch();
    void deletedLineAtRetryDeadlineCannotResubmit();
    void publicLineSurvivesPrivateLockAndNeverBecomesPrivate();
    void versionThreePersistsOnlyOptInAndMigratesOldRulesOff();
    void subUlpBoundaryAndNoticeKeepExactLinePrice();
    void phraseExactPriceTextIsValidatedWithoutDoubleFallback_data();
    void phraseExactPriceTextIsValidatedWithoutDoubleFallback();
    void stockLineUsesTwoEligibleActualBarsWithoutStudyWarmup_data();
    void stockLineUsesTwoEligibleActualBarsWithoutStudyWarmup();
    void linePhrasePreservesPublicUnicodeCurrency_data();
    void linePhrasePreservesPublicUnicodeCurrency();
    void linePhrasePreservesVerifiedBinanceIdentifierAtPairLimit();
    void invalidatedLineLaneCannotRetryOldContact();
    void invalidSourceAtRetryDeadlineCannotSubmitOldContact();
};
void HorizontalAlertDraft::twoActualClosedBarsWorkWithoutStudyWarmup(){
    qint64 now=Base+Minute+1000;Sink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(configure(engine));const auto demands=engine.demands();QCOMPARE(demands.size(),1);QVERIFY(demands.first().horizontal);QVERIFY(!demands.first().pullback&&!demands.first().results&&!demands.first().whales&&!demands.first().privateOnly);
    engine.beginLane(Coin,Minute,7,true);engine.observe(series(1,now),ChartStudySet{},7);QVERIFY(sink.posted.isEmpty());now+=Minute;auto next=series(2,now);touch(next.candles.last());engine.observe(next,ChartStudySet{},7);QCOMPARE(sink.posted.size(),1);QVERIFY(!sink.posted.first().privateData);QVERIFY(sink.posted.first().body.contains("100"));engine.observe(next,ChartStudySet{},7);QCOMPARE(sink.posted.size(),1);
}
void HorizontalAlertDraft::inclusiveTouchEpisodeRearmsOnlyAfterActualNonTouch(){
    qint64 now=Base+Minute+1000;Sink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(configure(engine));engine.beginLane(Coin,Minute,7,true);engine.observe(series(1,now),{},7);
    now+=Minute;auto next=series(2,now);touch(next.candles.last());next.candles.last().high="100";engine.observe(next,{},7);QCOMPARE(sink.posted.size(),1);now+=Minute;next=series(3,now);touch(next.candles[1]);next.candles[1].high="100";touch(next.candles[2]);engine.observe(next,{},7);QCOMPARE(sink.posted.size(),1);now+=Minute;next=series(4,now);touch(next.candles[1]);next.candles[1].high="100";touch(next.candles[2]);engine.observe(next,{},7);QCOMPARE(sink.posted.size(),1);now+=Minute;next=series(5,now);touch(next.candles[1]);next.candles[1].high="100";touch(next.candles[2]);touch(next.candles[4]);next.candles[4].low="100";next.candles[4].open=next.candles[4].close="100";engine.observe(next,{},7);QCOMPARE(sink.posted.size(),2);
}
void HorizontalAlertDraft::gapJumpAndFormingCannotBecomeObservedTouch(){
    qint64 now=Base+Minute+1000;Sink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(configure(engine));engine.beginLane(Coin,Minute,7,true);auto initial=series(1,now);engine.observe(initial,{},7);initial.forming=MarketCandle{Base+Minute,0,"99","110","90","105","1"};engine.observe(initial,{},7);QVERIFY(sink.posted.isEmpty());
    now+=Minute;auto jump=series(2,now);jump.candles.last()={Base+Minute,0,"102","103","101","102","1"};engine.observe(jump,{},7);QVERIFY(sink.posted.isEmpty());now+=2*Minute;auto missing=series(4,now);missing.candles.removeAt(2);missing.candles[1]=jump.candles.last();touch(missing.candles.last());engine.observe(missing,{},7);QVERIFY(sink.posted.isEmpty());
}
void HorizontalAlertDraft::deletedLineAtRetryDeadlineCannotResubmit(){
    qint64 now=Base+Minute+1000;Sink sink;sink.autoAck=false;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(configure(engine));engine.beginLane(Coin,Minute,7,true);engine.observe(series(1,now),{},7);now+=Minute;auto next=series(2,now);touch(next.candles.last());engine.observe(next,{},7);QCOMPARE(sink.posted.size(),1);const auto notice=sink.posted.first();emit sink.submissionFinished(notice.identifier,notice.privateEpoch,false);now+=5000;QVERIFY(engine.setHorizontalLines({}));engine.tick();QCOMPARE(sink.posted.size(),1);QVERIFY(engine.demands().isEmpty());emit sink.submissionFinished(notice.identifier,notice.privateEpoch,true);QCOMPARE(sink.posted.size(),1);
}
void HorizontalAlertDraft::publicLineSurvivesPrivateLockAndNeverBecomesPrivate(){
    qint64 now=Base+Minute+1000;Sink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(configure(engine));engine.setPrivateEnabled(true);engine.setHoldings({Coin},now,engine.privateEpoch());engine.beginLane(Coin,Minute,7,true);engine.observe(series(1,now),{},7);engine.clearPrivate();now+=Minute;auto next=series(2,now);touch(next.candles.last());engine.observe(next,{},7);QCOMPARE(sink.posted.size(),1);QVERIFY(!sink.posted.first().privateData);QVERIFY(sink.posted.first().identifier.startsWith("keefi.g8.public."));auto bad=line();bad.listId="@holdings";QVERIFY(!engine.setHorizontalLines({bad}));
}
void HorizontalAlertDraft::versionThreePersistsOnlyOptInAndMigratesOldRulesOff(){
    Sink sink;ChartAlertEngine engine(&sink);QVERIFY(configure(engine));const auto state=engine.publicState();auto json=QJsonDocument::fromJson(state).object();QCOMPARE(json["version"].toInt(),3);QVERIFY(engine.rule("public-lines").horizontal);QVERIFY(!engine.rule("@holdings").horizontal);QVERIFY(!state.contains(line().drawingId.toUtf8()));QVERIFY(!state.contains("price"));QVERIFY(!state.contains("identity"));
    json["version"]=2;auto rules=json["rules"].toArray();for(qsizetype i=0;i<rules.size();++i){auto rule=rules[i].toObject();rule.remove("horizontal");rules[i]=rule;}json["rules"]=rules;QVERIFY(engine.restorePublicState(QJsonDocument(json).toJson()));QVERIFY(!engine.rule("public-lines").horizontal);QVERIFY(engine.demands().isEmpty());const auto before=engine.publicState();auto malformed=rules;auto entry=malformed[0].toObject();entry["horizontal"]=true;malformed[0]=entry;json["rules"]=malformed;QVERIFY(!engine.restorePublicState(QJsonDocument(json).toJson()));QCOMPARE(engine.publicState(),before);
}
void HorizontalAlertDraft::subUlpBoundaryAndNoticeKeepExactLinePrice(){
    qint64 now=Base+Minute+1000;Sink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(configure(engine));auto precise=line();precise.price="100."+QString(39,'0')+"1";precise.revision=QString(64,'b');QVERIFY(engine.setHorizontalLines({precise}));engine.beginLane(Coin,Minute,7,true);engine.observe(series(1,now),{},7);now+=Minute;auto next=series(2,now);next.candles.last().high="100";engine.observe(next,{},7);QVERIFY(sink.posted.isEmpty());now+=Minute;next=series(3,now);next.candles[1].high="100";next.candles.last().high=precise.price;next.candles.last().close="100";engine.observe(next,{},7);QCOMPARE(sink.posted.size(),1);QVERIFY(sink.posted.first().body.contains(precise.price));
}
void HorizontalAlertDraft::phraseExactPriceTextIsValidatedWithoutDoubleFallback_data(){
    QTest::addColumn<QString>("text");QTest::addColumn<bool>("accepted");QTest::newRow("exact-fraction")<<("100."+QString(39,'0')+"1")<<true;QTest::newRow("original-trailing-zeros")<<QString("100.125000")<<true;QTest::newRow("valid-512chars")<<("1."+QString(510,'0'))<<true;QTest::newRow("zero")<<QString("0")<<false;QTest::newRow("negative")<<QString("-1")<<false;QTest::newRow("leadingzero")<<QString("01")<<false;QTest::newRow("linefeed")<<QString("100\n")<<false;QTest::newRow("exponent")<<QString("1e2")<<false;QTest::newRow("abovebound")<<("1"+QString(101,'0'))<<false;QTest::newRow("belowbound")<<("0."+QString(100,'0')+"1")<<false;QTest::newRow("oversize-513chars")<<("1."+QString(511,'0'))<<false;
}
void HorizontalAlertDraft::phraseExactPriceTextIsValidatedWithoutDoubleFallback(){
    QFETCH(QString,text);QFETCH(bool,accepted);ChartPhrasePack pack;ChartPhraseFacts facts;facts.symbol="SYN";facts.currency="KRW";facts.intervalMs=Minute;facts.samples=2;facts.priceText=text;facts.price=100; // A valid legacy double must never rescue invalid present exact text.
    const auto result=pack.render(ChartPhraseKind::Line,facts,false);QCOMPARE(result.available,accepted);if(accepted){QVERIFY(result.body.contains(text));facts.price.reset();const auto exactOnly=pack.render(ChartPhraseKind::Line,facts,false);QVERIFY(exactOnly.available);QVERIFY(exactOnly.body.contains(text));}else QVERIFY(result.body.isEmpty());
}
void HorizontalAlertDraft::stockLineUsesTwoEligibleActualBarsWithoutStudyWarmup_data(){
    QTest::addColumn<QString>("fault");for(const auto* value:{"valid","normal-closure","delayed","loading","calendar-unknown","adjustment-unknown","wrong-source","wrong-currency","ineligible","vi","disconnected","unknown-session","future-completion","stale"})QTest::newRow(value)<<QString(value);
}
void HorizontalAlertDraft::stockLineUsesTwoEligibleActualBarsWithoutStudyWarmup(){
    QFETCH(QString,fault);qint64 now=Base+Minute+1000;Sink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});const WatchlistIdentity stock{"stock","KR","0101N0"};QVERIFY(engine.setPublicLists({{"public-lines",{stock}}}));auto rule=engine.rule("public-lines");rule.enabled=true;rule.horizontal=true;rule.pullback=rule.results=rule.whales=false;rule.cooldownMs=0;QVERIFY(engine.setRule(rule));auto alert=line();alert.identity=stock;QVERIFY(engine.setHorizontalLines({alert}));
    const auto input=[&](int count){StockChartSnapshot value;value.series=series(count,now);value.series.identity=stock.key();value.series.asset=stock.code;value.series.sourceUrl=StockChartSource::request(StockChartRead::Candles,stock,Minute).url.toString();value.source=StockSourceKind::Toss;value.adjustment=StockAdjustment::SourceAdjusted;value.calendarAvailable=value.realtimeAllowed=value.intradayAllowed=true;value.timeline.adjustedComparable=true;
        for(const auto& candle:value.series.candles){ChartStudyTimelineRow row;row.startsAtMs=candle.startsAtMs;row.completesAtMs=candle.startsAtMs+Minute;row.tradingDate=QDateTime::fromMSecsSinceEpoch(candle.startsAtMs,QTimeZone("UTC")).date();row.session=StockSessionKind::Regular;row.eligible=true;row.connectsPrevious=!value.timeline.rows.isEmpty();value.timeline.rows.append(row);}return value;};
    engine.beginLane(stock,Minute,11,true);engine.observeStock(input(1),11);QVERIFY(sink.posted.isEmpty());now+=Minute;auto next=input(2);touch(next.series.candles.last());QVERIFY(!next.studies.available);
    if(fault=="normal-closure"){now+=3*86400000LL;next.series.receivedAtMs=now;next.series.candles.last().startsAtMs+=3*86400000LL;next.timeline.rows.last().startsAtMs=next.series.candles.last().startsAtMs;next.timeline.rows.last().completesAtMs=next.series.candles.last().startsAtMs+Minute;next.timeline.rows.last().tradingDate=next.timeline.rows.first().tradingDate.addDays(3);}
    if(fault=="delayed")next.delayed=true;if(fault=="loading")next.loading=true;if(fault=="calendar-unknown")next.calendarAvailable=false;if(fault=="adjustment-unknown")next.adjustment=StockAdjustment::Unknown;if(fault=="wrong-source")next.source=StockSourceKind::None;if(fault=="wrong-currency")next.series.currency="USD";if(fault=="ineligible")next.timeline.rows.last().eligible=false;if(fault=="vi")next.timeline.rows.last().exclusions.append(StockBarExclusion::VolatilityInterruption);if(fault=="disconnected")next.timeline.rows.last().connectsPrevious=false;if(fault=="unknown-session")next.timeline.rows.last().session=StockSessionKind::Unknown;if(fault=="future-completion")next.timeline.rows.last().completesAtMs=now+1;if(fault=="stale")now+=90001;
    engine.observeStock(next,11);QCOMPARE(sink.posted.size(),fault=="valid"||fault=="normal-closure"?1:0);
}
void HorizontalAlertDraft::linePhrasePreservesPublicUnicodeCurrency_data(){
    QTest::addColumn<QString>("currency");QTest::addColumn<bool>("accepted");QTest::newRow("chinese")<<QString::fromUtf8("合成")<<true;QTest::newRow("combining-mark")<<QString::fromUtf8("E\u0301")<<true;QTest::newRow("astral-forty")<<QString::fromUtf8("\xF0\x90\x90\x80").repeated(40)<<true;QTest::newRow("control")<<QString("USD\n")<<false;QTest::newRow("format")<<QString::fromUtf8("US\u200dD")<<false;QTest::newRow("url")<<QString("USD/FAKE")<<false;
}
void HorizontalAlertDraft::linePhrasePreservesPublicUnicodeCurrency(){
    QFETCH(QString,currency);QFETCH(bool,accepted);ChartPhrasePack pack;ChartPhraseFacts facts;facts.symbol="SYN";facts.currency=currency;facts.intervalMs=Minute;facts.samples=2;facts.priceText="100.125000";const auto rendered=pack.render(ChartPhraseKind::Line,facts,false);QCOMPARE(rendered.available,accepted);if(accepted)QVERIFY(rendered.body.contains(currency));
}
void HorizontalAlertDraft::linePhrasePreservesVerifiedBinanceIdentifierAtPairLimit(){
    ChartPhrasePack pack;ChartPhraseFacts facts;facts.symbol=QString::fromUtf8("\xF0\x90\x90\x80").repeated(36);facts.currency="USDT";facts.intervalMs=Minute;facts.samples=2;facts.priceText="100";const auto rendered=pack.render(ChartPhraseKind::Line,facts,false);QVERIFY(rendered.available);QVERIFY(rendered.body.contains(facts.symbol));
}
void HorizontalAlertDraft::invalidatedLineLaneCannotRetryOldContact(){
    qint64 now=Base+Minute+1000;Sink sink;sink.autoAck=false;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(configure(engine));engine.beginLane(Coin,Minute,7,true);engine.observe(series(1,now),{},7);now+=Minute;auto next=series(2,now);touch(next.candles.last());engine.observe(next,{},7);QCOMPARE(sink.posted.size(),1);const auto notice=sink.posted.first();emit sink.submissionFinished(notice.identifier,0,false);engine.invalidateLane(Coin,Minute,7);now+=5000;engine.tick();QCOMPARE(sink.posted.size(),1);
}
void HorizontalAlertDraft::invalidSourceAtRetryDeadlineCannotSubmitOldContact(){
    qint64 now=Base+Minute+1000;Sink sink;sink.autoAck=false;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(configure(engine));engine.beginLane(Coin,Minute,7,true);engine.observe(series(1,now),{},7);now+=Minute;auto next=series(2,now);touch(next.candles.last());engine.observe(next,{},7);QCOMPARE(sink.posted.size(),1);const auto notice=sink.posted.first();emit sink.submissionFinished(notice.identifier,0,false);now+=5000;next.sourceUrl="https://example.invalid/synthetic-wrong-source";engine.observe(next,{},7);QCOMPARE(sink.posted.size(),1);
}
QTEST_GUILESS_MAIN(HorizontalAlertDraft)
#include "tst_chart_horizontal_alerts.moc"
