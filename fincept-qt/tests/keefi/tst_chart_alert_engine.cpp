#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartPhrasePack.h"
#include "keefi/SystemNotificationSink.h"
#include "keefi/StockChartSource.h"
#include "keefi/StockChartTypes.h"
#include "keefi/CoinFlowStudies.h"
#include "keefi/MarketFlowPresentation.h"
#include <QTest>
#include <QSignalSpy>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QTimeZone>

using namespace keefi;
namespace {
constexpr qint64 Base=1728000000000,Minute=60000;
const WatchlistIdentity Coin{QStringLiteral("coin"),QStringLiteral("upbit"),QStringLiteral("KRW-SYN")};
const WatchlistIdentity Future{QStringLiteral("usdm_futures"),QStringLiteral("binance"),QStringLiteral("SYNUSDT")};
class FakeSink final:public SystemNotificationSink {
  public:
    SystemNoticePermission permissionValue=SystemNoticePermission::Granted;
    QList<SystemNotice> posted;QStringList removed;QList<QPair<quint64,bool>> contexts;
    int refreshes=0,requests=0,stops=0;bool automaticSuccess=true;
    std::function<void(const SystemNotice&)> duringPost;
    SystemNoticePermission permission() const override{return permissionValue;}
    QString status() const override{return QStringLiteral("Synthetic system sink");}
    void refreshPermission() override{++refreshes;}
    void requestAuthorization() override{++requests;}
    void post(const SystemNotice& notice) override{posted.append(notice);if(duringPost)duringPost(notice);
        if(automaticSuccess)emit submissionFinished(notice.identifier,notice.privateEpoch,true);}
    void remove(const QStringList& ids) override{removed.append(ids);}
    void setPrivateContext(quint64 epoch,bool enabled) override{contexts.append({epoch,enabled});}
    void stop() override{++stops;}
};
MarketCandleSeries snapshot(int count,qint64 now,qint64 interval=Minute,bool repeatedWaves=true){
    MarketCandleSeries series;series.available=true;series.identity=Coin.key();series.asset="SYN";series.currency="KRW";
    series.intervalMs=interval;series.sourceUrl=MarketCandles::request(Coin.key(),interval).url.toString();series.receivedAtMs=now;
    for(int i=0;i<count;++i){const auto at=Base+i*interval;series.candles.append({at,at+1000,"100","101","99","100","0"});}
    if(count>10)series.candles[10].low="90";if(count>11)series.candles[11].high="120";
    if(repeatedWaves)for(int i=24;i<count;i+=6){series.candles[i].low=QString::number(80-10*((i-24)/6));
        if(i+1<count)series.candles[i+1].high=QString::number(130+10*((i-24)/6));}
    return series;
}
void observe(ChartAlertEngine& engine,const MarketCandleSeries& series,quint64 generation=1){
    engine.observe(series,keefiChartStudies(series.candles,series.intervalMs),generation);
}
bool publicRule(ChartAlertEngine& engine,qint64 interval=Minute,bool results=true){
    if(!engine.setPublicLists({{QStringLiteral("public-a"),{Coin}}}))return false;
    ChartAlertRule rule;rule.listId="public-a";rule.enabled=true;rule.intervalMs=interval;rule.cooldownMs=0;rule.results=results;
    return engine.setRule(rule);
}
bool holdingRule(ChartAlertEngine& engine,qint64 now,const WatchlistIdentity& identity=Coin){
    engine.setPrivateEnabled(true);if(identity.market!="usdm_futures")engine.setHoldings({identity},now,engine.privateEpoch());
    ChartAlertRule rule;rule.listId="@holdings";rule.enabled=true;rule.intervalMs=Minute;rule.cooldownMs=0;rule.liquidation=true;
    return engine.setRule(rule);
}
}
class ChartAlertEngineTest final:public QObject {
    Q_OBJECT
  private slots:
    void publicMarketStateChangePostsOnceWithoutPrivateContext();
    void publicContractFlowChangesUseCanonicalDirections();
    void publicMarketFlagsPersistSeparatelyAndRejectPrivateFields();
    void publicMarketAssetSwitchCancelsAllPending();
    void publicMarketClocksAndUnavailableStates_data();
    void publicMarketClocksAndUnavailableStates();
    void publicContractFlowExpiredWindowCannotRetry();
    void publicMarketSuppressionDoesNotCatchUp_data();
    void publicMarketSuppressionDoesNotCatchUp();
    void publicMarketSharesChartDailyCapacity();
    void publicMarketRetryRemainsPlainAndPublicAfterLock();
    void publicMarketInvalidationAtRetryDeadlineCancels_data();
    void publicMarketInvalidationAtRetryDeadlineCancels();
    void publicMarketUnavailableGapRebaselines();
    void constructionIsPassiveAndHoldingsDefaultEnabled();
    void firstSeedNeverBurstsAndNewCompletedSignalDeduplicates();
    void trustAndFreshness_data();
    void trustAndFreshness();
    void reconnectBaselinesAndDelayedDailyCannotAlert();
    void measuredResultsIncludeLossAndFlat_data();
    void measuredResultsIncludeLossAndFlat();
    void quietSuppressionIsConsumedAndPendingCapIsReserved();
    void muteUsesExplicitTimezoneAndCivilMidnight();
    void failedSchedulingHasOneBoundedRetry();
    void disablingOnlyThePendingKindCancelsItsRetry();
    void retryUsesCurrentDripSettingAndRetainsObservedFacts();
    void lockPurgesPrivatePendingDeliveredAndLateCallbacks();
    void reentrantLockCannotResurrectPrivatePost();
    void publicOverlapRemainsPublicThroughPrivateLock();
    void globalOwnCategoryActionsWorkAfterLock();
    void liquidationUsesSeriousPlainCurrentRisk();
    void liquidationPreservesRawProviderSide_data();
    void liquidationPreservesRawProviderSide();
    void freshOwnedFuturesRiskNeedsNoSpotBalanceMembership();
    void spotIdentityNeverProvesFuturesRiskEvenWhenHeld();
    void liquidationBatchAcceptsVerifiedOwnedRiskWithoutSpot();
    void absentPositionBatchCancelsPendingRiskRetry();
    void expiredSpotHoldingsDoNotRevokeFreshFuturesRisk();
    void absentPositionBatchAtRetryDeadlineCannotResubmit();
    void onlyPublicConfigurationCanPersist();
    void stockRulesRequestTypedCompletedCandles();
    void stockSignalsRequireCalendarEligibilityAndRealtimeSource_data();
    void stockSignalsRequireCalendarEligibilityAndRealtimeSource();
    void whaleOptInUsesIndependentCompletedMinuteDemandAndStrictPublicMigration();
    void completedCoinFlowUsesWhalePhraseWithoutPullbackOrHistoricalBurst();
    void canonicalCandleOnlyVolumeCannotBorrowForgedFlowMetadata_data();
    void canonicalCandleOnlyVolumeCannotBorrowForgedFlowMetadata();
    void canonicalNativeMinutePreservesVerifiedUnicodeAsset_data();
    void canonicalNativeMinutePreservesVerifiedUnicodeAsset();
    void additiveNoticeMetadataKeepsLegacySignalCallersCompatible();
};
void ChartAlertEngineTest::canonicalNativeMinutePreservesVerifiedUnicodeAsset_data(){
    QTest::addColumn<QString>("baseAsset");
    QTest::newRow("CJK-exact")<<QStringLiteral("合成");
    QTest::newRow("combining-without-normalization")<<QStringLiteral("E\u0301");
    // The complete base+USDT symbol is exactly 40 Unicode scalars. Its base
    // alone uses 72 UTF-16 code units, exposing a separate phrase-size bound.
    QTest::newRow("astral-full-symbol-40-scalars")<<QString::fromUcs4(U"\U00010400").repeated(36);
}
void ChartAlertEngineTest::canonicalNativeMinutePreservesVerifiedUnicodeAsset(){
    QFETCH(QString,baseAsset);qint64 now=Base+21*Minute+1000;FakeSink sink;ChartPhrasePack phrases;
    ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    const WatchlistIdentity identity{"coin","binance",baseAsset+"USDT"};
    QVERIFY(engine.setPublicLists({{"unicode-native-coin",{identity}}}));
    auto rule=engine.rule("unicode-native-coin");rule.enabled=true;rule.pullback=rule.results=false;rule.whales=true;rule.cooldownMs=0;QVERIFY(engine.setRule(rule));
    const auto input=[&](int count){CoinFlowSnapshot out;out.identity=identity.key();MarketCandleSeries canonical;canonical.available=true;canonical.identity=out.identity;
        canonical.asset=baseAsset;canonical.currency="USDT";canonical.intervalMs=Minute;canonical.sourceUrl=MarketCandles::request(out.identity,Minute).url.toString();canonical.receivedAtMs=now;
        for(int i=0;i<count;++i){CoinFlowClosedMinute row;row.candle={Base+i*Minute,0,"100","101","99","100",i==20?"40":"10"};
            row.completesAtMs=Base+(i+1)*Minute;row.knownAtMs=row.decisionAtMs=row.completesAtMs+1000;row.connectsPrevious=i>0;row.liveEligible=true;
            row.evidence={"합성 검증된 Unicode C3 원천",canonical.sourceUrl,0,row.knownAtMs,row.knownAtMs};canonical.candles.append(row.candle);out.minutes.append(row);}
        out.candleSeries=canonical;return out;};
    QVERIFY(!MarketCandles::request(identity.key(),Minute).url.isEmpty());
    engine.beginLane(identity,Minute,81,true);engine.observeCoinFlow(input(20),81);QVERIFY(sink.posted.isEmpty());
    engine.observeCoinFlow(input(21),81);QCOMPARE(sink.posted.size(),1);
    QVERIFY(sink.posted.first().title.contains(QStringLiteral("큰손·반전")));QVERIFY(sink.posted.first().body.contains(baseAsset));
    if(baseAsset==QStringLiteral("E\u0301"))QVERIFY(!sink.posted.first().body.contains(QStringLiteral("\u00c9")));
    engine.observeCoinFlow(input(21),81);QCOMPARE(sink.posted.size(),1);QCOMPARE(sink.requests,0);
}
void ChartAlertEngineTest::canonicalCandleOnlyVolumeCannotBorrowForgedFlowMetadata_data(){
    QTest::addColumn<QString>("fault");QTest::addColumn<int>("expected");
    for(const auto* fault:{"valid-empty-primary","forged-primary-base","wrong-source","wrong-identity","wrong-asset","wrong-currency","body-mismatch","invalid-ohlc","future-receipt"})
        QTest::newRow(fault)<<QString(fault)<<((QString(fault)=="valid-empty-primary"||QString(fault)=="forged-primary-base")?1:0);
}
void ChartAlertEngineTest::canonicalCandleOnlyVolumeCannotBorrowForgedFlowMetadata(){
    QFETCH(QString,fault);QFETCH(int,expected);qint64 now=Base+21*Minute+1000;FakeSink sink;ChartPhrasePack phrases;
    ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);const WatchlistIdentity identity{"coin","binance","SYNUSDT"};
    QVERIFY(engine.setPublicLists({{"synthetic-native-coin",{identity}}}));auto rule=engine.rule("synthetic-native-coin");rule.enabled=true;rule.pullback=rule.results=false;rule.whales=true;rule.cooldownMs=0;QVERIFY(engine.setRule(rule));
    const auto input=[&](int count){CoinFlowSnapshot out;out.identity=identity.key();MarketCandleSeries canonical;canonical.available=true;canonical.identity=out.identity;canonical.asset="SYN";canonical.currency="USDT";
        canonical.intervalMs=Minute;canonical.sourceUrl=MarketCandles::request(identity.key(),Minute).url.toString();canonical.receivedAtMs=now;
        for(int i=0;i<count;++i){CoinFlowClosedMinute row;row.candle={Base+i*Minute,0,"100","101","99","100",i==20?"40":"10"};row.completesAtMs=Base+(i+1)*Minute;
            row.knownAtMs=row.decisionAtMs=row.completesAtMs+1000;row.liveEligible=true;row.connectsPrevious=i>0;row.evidence={"합성 검증된 C3 1분",canonical.sourceUrl,0,row.knownAtMs,row.knownAtMs};
            canonical.candles.append(row.candle);out.minutes.append(row);}out.candleSeries=canonical;return out;};
    engine.beginLane(identity,Minute,71,true);engine.observeCoinFlow(input(20),71);QVERIFY(sink.posted.isEmpty());auto latest=input(21);
    if(fault=="forged-primary-base"){auto& i=latest.primary.instrument;i.venue=CoinFlowVenue::BinanceSpot;i.grain=CoinFlowGrain::SpotAggregate;i.identity=identity.key();i.symbol="SYNUSDT";
        i.baseAsset="FAKE";i.quoteAsset="USDT";i.status="TRADING";i.verified=true;i.evidence={"합성 잘못된 원천 메타데이터","https://api.binance.com/api/v3/exchangeInfo",0,now,now};}
    if(fault=="wrong-source")latest.candleSeries->sourceUrl="https://example.invalid/synthetic-only";
    if(fault=="wrong-identity")latest.candleSeries->identity="coin:binance:OTHERUSDT";
    if(fault=="wrong-asset")latest.candleSeries->asset="FAKE";
    if(fault=="wrong-currency")latest.candleSeries->currency="KRW";
    if(fault=="body-mismatch")latest.candleSeries->candles.last().volume="777";
    if(fault=="invalid-ohlc"){latest.candleSeries->candles.last().high="90";latest.minutes.last().candle.high="90";}
    if(fault=="future-receipt")latest.candleSeries->receivedAtMs=now+1;
    engine.observeCoinFlow(latest,71);QCOMPARE(sink.posted.size(),expected);
    if(expected){QVERIFY(sink.posted.first().title.contains(QStringLiteral("큰손·반전")));QVERIFY(sink.posted.first().body.contains("SYN"));QVERIFY(!sink.posted.first().body.contains("FAKE"));
        engine.observeCoinFlow(latest,71);QCOMPARE(sink.posted.size(),1);}
    QCOMPARE(sink.requests,0);
}
void ChartAlertEngineTest::whaleOptInUsesIndependentCompletedMinuteDemandAndStrictPublicMigration(){
    FakeSink sink;ChartAlertEngine engine(&sink);QVERIFY(engine.setPublicLists({{"public-a",{Coin,{"stock","US","SYN"}}}}));
    auto rule=engine.rule("public-a");QVERIFY(!rule.whales);QVERIFY(!engine.rule("@holdings").whales);
    rule.enabled=true;rule.pullback=true;rule.results=false;rule.intervalMs=900000;rule.whales=true;QVERIFY(engine.setRule(rule));
    const auto demands=engine.demands();int minute=0,study=0;
    for(const auto& demand:demands){if(demand.identity==Coin&&demand.intervalMs==Minute){QVERIFY(demand.whales);QVERIFY(!demand.pullback);++minute;}
        if(demand.intervalMs==900000){QVERIFY(demand.pullback);QVERIFY(!demand.whales);++study;}}
    QCOMPARE(minute,1);QCOMPARE(study,2); // The stock row keeps its ordinary study demand, never a coin-flow demand.
    const auto state=engine.publicState();auto object=QJsonDocument::fromJson(state).object();QCOMPARE(object.value("version").toInt(),3);
    ChartAlertEngine restored(&sink);QVERIFY(restored.setPublicLists({{"public-a",{Coin,{"stock","US","SYN"}}}}));QVERIFY(restored.restorePublicState(state));QVERIFY(restored.rule("public-a").whales);
    auto legacy=object;legacy["version"]=1;auto legacyRules=legacy.value("rules").toArray();
    for(qsizetype i=0;i<legacyRules.size();++i){auto entry=legacyRules[i].toObject();entry.remove("whales");entry.remove("horizontal");legacyRules[i]=entry;}legacy["rules"]=legacyRules;
    QVERIFY(restored.restorePublicState(QJsonDocument(legacy).toJson()));QVERIFY(!restored.rule("public-a").whales);
    const auto before=restored.publicState();auto malformed=legacy;auto invalidRules=legacyRules;auto invalid=invalidRules[0].toObject();invalid["whales"]=true;invalidRules[0]=invalid;malformed["rules"]=invalidRules;
    QVERIFY(!restored.restorePublicState(QJsonDocument(malformed).toJson()));QCOMPARE(restored.publicState(),before);
    malformed=object;invalidRules=malformed.value("rules").toArray();invalid=invalidRules[0].toObject();invalid["whales"]="true";invalidRules[0]=invalid;malformed["rules"]=invalidRules;
    QVERIFY(!restored.restorePublicState(QJsonDocument(malformed).toJson()));QCOMPARE(restored.publicState(),before);
    QVERIFY(!state.contains("receivedAt"));QVERIFY(!state.contains("trades"));QVERIFY(!state.contains("quantity"));QCOMPARE(sink.requests,0);
}
void ChartAlertEngineTest::completedCoinFlowUsesWhalePhraseWithoutPullbackOrHistoricalBurst(){
    qint64 now=Base+21*Minute+1000;FakeSink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(engine.setPublicLists({{"public-a",{Coin}}}));auto rule=engine.rule("public-a");rule.enabled=true;rule.pullback=false;rule.results=false;rule.whales=true;rule.cooldownMs=0;QVERIFY(engine.setRule(rule));
    const auto input=[&](int count){CoinFlowSnapshot out;out.identity=Coin.key();auto& instrument=out.primary.instrument;instrument.venue=CoinFlowVenue::UpbitSpot;instrument.grain=CoinFlowGrain::UpbitTrade;
        instrument.identity=Coin.key();instrument.symbol="KRW-SYN";instrument.baseAsset="SYN";instrument.quoteAsset="KRW";instrument.verified=true;
        instrument.evidence={"업비트 합성 공개 종목 카탈로그","https://api.upbit.com/v1/market/all",0,now,now};out.primary.evidence=instrument.evidence;
        for(int i=0;i<count;++i){CoinFlowClosedMinute row;row.candle={Base+i*Minute,0,"100","100","100","100",i==20?"40":"10"};
            row.completesAtMs=Base+(i+1)*Minute;row.knownAtMs=row.decisionAtMs=row.completesAtMs+1000;row.connectsPrevious=i>0;row.liveEligible=true;
            row.evidence={"업비트 합성 완료1분",MarketCandles::request(Coin.key(),Minute).url.toString(),0,row.knownAtMs,row.knownAtMs};out.minutes.append(row);}return out;};
    engine.beginLane(Coin,Minute,41,true);engine.observeCoinFlow(input(20),41);QVERIFY(sink.posted.isEmpty());
    engine.observeCoinFlow(input(21),41);QCOMPARE(sink.posted.size(),1);QVERIFY(sink.posted.first().title.contains(QStringLiteral("큰손·반전")));
    QVERIFY(sink.posted.first().body.contains(QStringLiteral("거래량 급증")));QVERIFY(!sink.posted.first().title.contains(QStringLiteral("눌림존")));
    engine.observeCoinFlow(input(21),41);QCOMPARE(sink.posted.size(),1);
    engine.beginLane(Coin,Minute,42,true);engine.observeCoinFlow(input(21),42);QCOMPARE(sink.posted.size(),1); // Reconnect seeds the latest decision, never bursts history.
    engine.observeCoinFlow(input(21),41);QCOMPARE(sink.posted.size(),1);QCOMPARE(sink.requests,0);
}
void ChartAlertEngineTest::stockRulesRequestTypedCompletedCandles(){
    qint64 now=Base;FakeSink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
    QVERIFY(engine.setPublicLists({{"stock-list",{{"stock","US","SYN"}}}}));
    ChartAlertRule rule;rule.listId="stock-list";rule.enabled=true;rule.intervalMs=Minute;
    QVERIFY(engine.setRule(rule));QCOMPARE(engine.demands().size(),1);
    QCOMPARE(engine.demands().first().identity.key(),QString("stock:US:SYN"));
}
void ChartAlertEngineTest::stockSignalsRequireCalendarEligibilityAndRealtimeSource_data(){
    QTest::addColumn<QString>("fault");QTest::addColumn<int>("expected");
    for(const auto* fault:{"valid","delayed","not-realtime","calendar-unknown","row-ineligible","wrong-source","wrong-generation","gap","wrong-timeline"})
        QTest::newRow(fault)<<QString(fault)<<(QString(fault)=="valid"?1:0);
}
void ChartAlertEngineTest::stockSignalsRequireCalendarEligibilityAndRealtimeSource(){
    QFETCH(QString,fault);QFETCH(int,expected);qint64 now=Base+26*Minute;FakeSink sink;ChartPhrasePack phrases;
    ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    const WatchlistIdentity id{"stock","US","SYN"};QVERIFY(engine.setPublicLists({{"stock-list",{id}}}));
    ChartAlertRule rule;rule.listId="stock-list";rule.enabled=true;rule.intervalMs=Minute;rule.cooldownMs=0;rule.results=false;QVERIFY(engine.setRule(rule));
    engine.beginLane(id,Minute,1,true);
    const auto make=[&](int count){StockChartSnapshot out;out.series=snapshot(count,now);out.series.identity=id.key();out.series.asset="SYN";out.series.currency="USD";
        out.series.sourceUrl=StockChartSource::request(StockChartRead::Candles,id,Minute).url.toString();
        out.source=StockSourceKind::Toss;out.adjustment=StockAdjustment::SourceAdjusted;out.realtimeAllowed=true;out.intradayAllowed=true;out.calendarAvailable=true;
        out.timeline.adjustedComparable=true;
        for(int i=0;i<count;++i){ChartStudyTimelineRow row;row.startsAtMs=out.series.candles[i].startsAtMs;row.completesAtMs=row.startsAtMs+Minute;
            row.eligible=true;row.connectsPrevious=i>0;row.missingSlotsBefore=0;row.session=StockSessionKind::Regular;row.tradingDate=QDate(2024,10,4);out.timeline.rows.append(row);}
        out.studies=keefiChartStudies(out.series.candles,Minute,out.timeline);return out;};
    engine.observeStock(make(26),1);QVERIFY(sink.posted.isEmpty());now+=Minute;auto out=make(27);quint64 generation=1;
    if(fault=="delayed")out.delayed=true;
    if(fault=="not-realtime")out.realtimeAllowed=false;
    if(fault=="calendar-unknown")out.calendarAvailable=false;
    if(fault=="row-ineligible")out.timeline.rows.last().eligible=false;
    if(fault=="wrong-source")out.series.sourceUrl="https://example.invalid/synthetic";
    if(fault=="wrong-generation")generation=2;
    if(fault=="gap")out.timeline.rows.last().connectsPrevious=false;
    if(fault=="wrong-timeline")out.timeline.rows.last().startsAtMs+=1;
    engine.observeStock(out,generation);QCOMPARE(sink.posted.size(),expected);
    engine.observeStock(out,generation);QCOMPARE(sink.posted.size(),expected);
}
void ChartAlertEngineTest::constructionIsPassiveAndHoldingsDefaultEnabled(){
    qint64 now=Base;FakeSink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
    QCOMPARE(sink.requests,0);QCOMPARE(sink.refreshes,0);QVERIFY(sink.posted.isEmpty());QVERIFY(engine.demands().isEmpty());
    const auto rule=engine.rule("@holdings");QVERIFY(rule.enabled);QVERIFY(rule.pullback);QVERIFY(rule.results);QVERIFY(rule.liquidation);
    QCOMPARE(engine.settings().dailyLimit,20);QVERIFY(engine.settings().dripEnabled);QVERIFY(!engine.settings().quietEnabled);
}
void ChartAlertEngineTest::firstSeedNeverBurstsAndNewCompletedSignalDeduplicates(){
    qint64 now=Base+26*Minute;FakeSink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(publicRule(engine));QCOMPARE(engine.demands().size(),1);engine.beginLane(Coin,Minute,1,true);
    observe(engine,snapshot(26,now));QVERIFY(sink.posted.isEmpty());
    now+=Minute;auto current=snapshot(27,now);observe(engine,current);QCOMPARE(sink.posted.size(),1);
    QVERIFY(!sink.posted[0].privateData);QVERIFY(sink.posted[0].identifier.startsWith("keefi.g8.public."));
    QVERIFY(!sink.posted[0].identifier.contains("SYN"));QVERIFY(!sink.posted[0].title.isEmpty());QVERIFY(!sink.posted[0].body.isEmpty());
    observe(engine,current);current.forming=MarketCandle{now,now+1,"100","101","99","100","9"};observe(engine,current);
    QCOMPARE(sink.posted.size(),1);QCOMPARE(sink.requests,0);
}
void ChartAlertEngineTest::trustAndFreshness_data(){
    QTest::addColumn<QString>("mutation");QTest::addColumn<int>("expected");
    for(const auto* value:{"valid","stale","future-receipt","wrong-source","wrong-identity","wrong-interval","old-generation","unavailable"})
        QTest::newRow(value)<<QString(value)<<(QString(value)=="valid"?1:0);
}
void ChartAlertEngineTest::trustAndFreshness(){
    QFETCH(QString,mutation);QFETCH(int,expected);qint64 now=Base+26*Minute;FakeSink sink;ChartPhrasePack phrases;
    ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);QVERIFY(publicRule(engine));engine.beginLane(Coin,Minute,2,true);
    observe(engine,snapshot(26,now),2);now+=Minute;auto current=snapshot(27,now);quint64 generation=2;
    if(mutation=="stale")current.receivedAtMs=now-90001;
    if(mutation=="future-receipt")current.receivedAtMs=now+1;
    if(mutation=="wrong-source")current.sourceUrl="https://unverified.invalid/candles";
    if(mutation=="wrong-identity")current.identity="coin:binance:SYNUSDT";
    if(mutation=="wrong-interval")current.intervalMs=300000;
    if(mutation=="old-generation")generation=1;
    if(mutation=="unavailable")current.available=false;
    observe(engine,current,generation);QCOMPARE(sink.posted.size(),expected);
}
void ChartAlertEngineTest::reconnectBaselinesAndDelayedDailyCannotAlert(){
    qint64 now=Base+26*Minute;FakeSink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(publicRule(engine));engine.beginLane(Coin,Minute,1,true);observe(engine,snapshot(26,now));
    engine.invalidateLane(Coin,Minute,1);engine.beginLane(Coin,Minute,2,true);now+=Minute;
    observe(engine,snapshot(27,now),1);observe(engine,snapshot(27,now),2);QVERIFY(sink.posted.isEmpty());
    now=Base+33*Minute;observe(engine,snapshot(33,now),2);QCOMPARE(sink.posted.size(),1);
    engine.beginLane(Coin,Minute,3,false);observe(engine,snapshot(33,now),3);now=Base+39*Minute;observe(engine,snapshot(39,now),3);
    QCOMPARE(sink.posted.size(),1);
}
void ChartAlertEngineTest::measuredResultsIncludeLossAndFlat_data(){
    QTest::addColumn<QString>("close");QTest::addColumn<QString>("expectedText");
    QTest::newRow("loss")<<QString("95")<<QString("-5");QTest::newRow("flat")<<QString("100")<<QString("0");
    QTest::newRow("gain")<<QString("105")<<QString("5");
}
void ChartAlertEngineTest::measuredResultsIncludeLossAndFlat(){
    QFETCH(QString,close);QFETCH(QString,expectedText);constexpr qint64 Hour=3600000;qint64 now=Base+44*Hour;FakeSink sink;ChartPhrasePack phrases;
    ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QSignalSpy accepted(&engine,&ChartAlertEngine::noticeSubmitted);engine.setPhrasePack(&phrases);QVERIFY(publicRule(engine,Hour));
    auto rule=engine.rule("public-a");rule.listId="public-a";rule.enabled=true;rule.intervalMs=Hour;rule.pullback=false;rule.results=true;rule.cooldownMs=0;QVERIFY(engine.setRule(rule));
    engine.beginLane(Coin,Hour,1,true);observe(engine,snapshot(44,now,Hour,false));now+=Hour;auto current=snapshot(45,now,Hour,false);
    auto& last=current.candles.last();last.open=last.close=close;last.high=QString::number(close.toDouble()+1);last.low=QString::number(close.toDouble()-1);
    const auto studies=keefiChartStudies(current.candles,Hour);QCOMPARE(studies.scorecard.samples,1);QCOMPARE(studies.scorecard.horizonMs,qint64(86400000));
    engine.observe(current,studies,1);QCOMPARE(sink.posted.size(),1);QVERIFY(sink.posted[0].body.contains(expectedText));
    const auto direction=close=="95"?MovementDirection::Down:close=="105"?MovementDirection::Up:MovementDirection::Flat;
    QCOMPARE(sink.posted.first().direction,direction);QVERIFY(!sink.posted.first().fixedRisk);
    QCOMPARE(accepted.size(),1);QCOMPARE(accepted.first().size(),6);QCOMPARE(accepted.first().at(4).value<MovementDirection>(),direction);QVERIFY(!accepted.first().at(5).toBool());
    const auto marker=direction==MovementDirection::Up?QStringLiteral("▲"):direction==MovementDirection::Down?QStringLiteral("▼"):QString{};
    if(!marker.isEmpty())QCOMPARE(sink.posted.first().title.count(marker),1);
}
void ChartAlertEngineTest::additiveNoticeMetadataKeepsLegacySignalCallersCompatible(){
    FakeSink sink;ChartAlertEngine engine(&sink);int oldCalls=0;connect(&engine,&ChartAlertEngine::noticeSubmitted,this,[&](QString,QString,QString,bool){++oldCalls;});QSignalSpy accepted(&engine,&ChartAlertEngine::noticeSubmitted);
    const SystemNotice legacy{"synthetic","Synthetic","Synthetic body",false,0};QCOMPARE(legacy.direction,MovementDirection::Unknown);QVERIFY(!legacy.fixedRisk);
    emit engine.noticeSubmitted("legacy","Synthetic","Legacy four arguments",false);QCOMPARE(oldCalls,1);QCOMPARE(accepted.size(),1);QCOMPARE(accepted.first().at(4).value<MovementDirection>(),MovementDirection::Unknown);QVERIFY(!accepted.first().at(5).toBool());
}
void ChartAlertEngineTest::quietSuppressionIsConsumedAndPendingCapIsReserved(){
    qint64 now=Base+26*Minute;FakeSink sink;sink.automaticSuccess=false;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(publicRule(engine));auto settings=engine.settings();settings.timeZoneId="Asia/Seoul";settings.quietEnabled=true;settings.quietStartMinute=settings.quietEndMinute=0;settings.dailyLimit=1;QVERIFY(engine.setSettings(settings));
    engine.beginLane(Coin,Minute,1,true);observe(engine,snapshot(26,now));now=Base+27*Minute;observe(engine,snapshot(27,now));QVERIFY(sink.posted.isEmpty());
    settings.quietEnabled=false;QVERIFY(engine.setSettings(settings));observe(engine,snapshot(27,now));QVERIFY(sink.posted.isEmpty());
    now=Base+33*Minute;observe(engine,snapshot(33,now));QCOMPARE(sink.posted.size(),1);
    now=Base+39*Minute;observe(engine,snapshot(39,now));QCOMPARE(sink.posted.size(),1);
    const auto notice=sink.posted.first();emit sink.submissionFinished(notice.identifier,notice.privateEpoch,true);observe(engine,snapshot(39,now));QCOMPARE(sink.posted.size(),1);
}
void ChartAlertEngineTest::muteUsesExplicitTimezoneAndCivilMidnight(){
    const QTimeZone zone("America/New_York");qint64 now=QDateTime(QDate(2026,11,1),QTime(0,30),zone).toMSecsSinceEpoch();FakeSink sink;
    ChartAlertEngine engine(&sink,nullptr,[&]{return now;});auto settings=engine.settings();settings.timeZoneId=zone.id();QVERIFY(engine.setSettings(settings));
    engine.muteForOneHour();QCOMPARE(engine.settings().mutedUntilMs,now+3600000);
    engine.muteForToday();const auto midnight=QDate(2026,11,2).startOfDay(zone).toMSecsSinceEpoch();QCOMPARE(engine.settings().mutedUntilMs,midnight);
    QVERIFY(midnight-now>24*3600000);
}
void ChartAlertEngineTest::failedSchedulingHasOneBoundedRetry(){
    qint64 now=Base+26*Minute;FakeSink sink;sink.automaticSuccess=false;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(publicRule(engine));engine.beginLane(Coin,Minute,1,true);observe(engine,snapshot(26,now));now+=Minute;observe(engine,snapshot(27,now));QCOMPARE(sink.posted.size(),1);
    const auto first=sink.posted.first();emit sink.submissionFinished(first.identifier,first.privateEpoch,false);
    now+=4999;engine.tick();QCOMPARE(sink.posted.size(),1);++now;engine.tick();QCOMPARE(sink.posted.size(),2);QCOMPARE(sink.posted.last().identifier,first.identifier);
    emit sink.submissionFinished(first.identifier,first.privateEpoch,false);now+=5000;engine.tick();QCOMPARE(sink.posted.size(),2);
}
void ChartAlertEngineTest::disablingOnlyThePendingKindCancelsItsRetry(){
    qint64 now=Base+26*Minute;FakeSink sink;sink.automaticSuccess=false;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(publicRule(engine));engine.beginLane(Coin,Minute,1,true);observe(engine,snapshot(26,now));now+=Minute;observe(engine,snapshot(27,now));QCOMPARE(sink.posted.size(),1);
    const auto notice=sink.posted.first();emit sink.submissionFinished(notice.identifier,notice.privateEpoch,false);
    auto rule=engine.rule("public-a");rule.pullback=false;rule.results=true;QVERIFY(engine.setRule(rule));QCOMPARE(engine.demands().size(),1);QVERIFY(engine.demands().first().results);
    now+=5000;engine.tick();QCOMPARE(sink.posted.size(),1);QVERIFY(sink.removed.contains(notice.identifier));
}
void ChartAlertEngineTest::retryUsesCurrentDripSettingAndRetainsObservedFacts(){
    auto object=QJsonDocument::fromJson(ChartPhrasePack::plainJson()).object();object["id"]="synthetic_retry";object["name"]=QStringLiteral("합성 재시도 문구");
    auto templates=object.value("templates").toObject();auto rows=templates.value("pullback").toArray();
    for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();row["id"]=QString("retry_%1").arg(i);row["text"]="RETRY_SYNTHETIC "+row.value("text").toString();rows[i]=row;}
    templates["pullback"]=rows;object["templates"]=templates;const auto raw=QJsonDocument(object).toJson(QJsonDocument::Compact);const auto validation=ChartPhrasePack::validate(raw);QVERIFY(validation.valid);
    qint64 now=Base+26*Minute;FakeSink sink;sink.automaticSuccess=false;ChartPhrasePack phrases;QVERIFY(phrases.activateReviewed(raw,validation.sha256));
    ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);QVERIFY(publicRule(engine));engine.beginLane(Coin,Minute,1,true);
    observe(engine,snapshot(26,now));now+=Minute;observe(engine,snapshot(27,now));QCOMPARE(sink.posted.size(),1);QVERIFY(sink.posted.first().body.contains("RETRY_SYNTHETIC"));
    const auto notice=sink.posted.first();emit sink.submissionFinished(notice.identifier,notice.privateEpoch,false);engine.setDripEnabled(false);now+=5000;engine.tick();
    QCOMPARE(sink.posted.size(),2);QCOMPARE(sink.posted.last().identifier,notice.identifier);QVERIFY(!sink.posted.last().body.contains("RETRY_SYNTHETIC"));
    QVERIFY(sink.posted.last().body.contains("27"));QVERIFY(sink.posted.last().body.contains(QStringLiteral("마감된 봉")));
}
void ChartAlertEngineTest::lockPurgesPrivatePendingDeliveredAndLateCallbacks(){
    qint64 now=Base+26*Minute;FakeSink sink;sink.automaticSuccess=false;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(holdingRule(engine,now));const auto epoch=engine.privateEpoch();QVERIFY(epoch>0);QCOMPARE(engine.demands().size(),1);QVERIFY(engine.demands()[0].privateOnly);
    engine.beginLane(Coin,Minute,1,true);observe(engine,snapshot(26,now));now+=Minute;observe(engine,snapshot(27,now));QCOMPARE(sink.posted.size(),1);
    const auto notice=sink.posted.first();QVERIFY(notice.privateData);QCOMPARE(notice.privateEpoch,epoch);QVERIFY(notice.identifier.startsWith("keefi.g8.private."));
    engine.clearPrivate();QVERIFY(engine.privateEpoch()>epoch);QVERIFY(engine.demands().isEmpty());QVERIFY(sink.removed.contains(notice.identifier));
    emit sink.submissionFinished(notice.identifier,epoch,true);engine.tick();QVERIFY(sink.removed.count(notice.identifier)>=2);
    engine.setPrivateEnabled(true);engine.setHoldings({Coin},now,epoch);QVERIFY(engine.demands().isEmpty());
    QVERIFY(!sink.contexts.isEmpty());QVERIFY(sink.contexts.last().first!=epoch);
}
void ChartAlertEngineTest::reentrantLockCannotResurrectPrivatePost(){
    qint64 now=Base+26*Minute;FakeSink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(holdingRule(engine,now));engine.beginLane(Coin,Minute,1,true);observe(engine,snapshot(26,now));
    sink.duringPost=[&](const SystemNotice& notice){engine.clearPrivate();QVERIFY(sink.removed.contains(notice.identifier));};
    now+=Minute;observe(engine,snapshot(27,now));QCOMPARE(sink.posted.size(),1);QVERIFY(engine.demands().isEmpty());
    QVERIFY(sink.removed.contains(sink.posted.first().identifier));
}
void ChartAlertEngineTest::publicOverlapRemainsPublicThroughPrivateLock(){
    qint64 now=Base+26*Minute;FakeSink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(publicRule(engine));QVERIFY(holdingRule(engine,now));QCOMPARE(engine.demands().size(),1);QVERIFY(!engine.demands()[0].privateOnly);
    engine.beginLane(Coin,Minute,1,true);observe(engine,snapshot(26,now));now+=Minute;observe(engine,snapshot(27,now));QCOMPARE(sink.posted.size(),1);QVERIFY(!sink.posted.first().privateData);
    const auto id=sink.posted.first().identifier;engine.clearPrivate();QCOMPARE(engine.demands().size(),1);QVERIFY(!sink.removed.contains(id));
    now=Base+33*Minute;observe(engine,snapshot(33,now));QCOMPARE(sink.posted.size(),2);QVERIFY(!sink.posted.last().privateData);
}
void ChartAlertEngineTest::globalOwnCategoryActionsWorkAfterLock(){
    qint64 now=Base;FakeSink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QSignalSpy disabled(&engine,&ChartAlertEngine::dripDisabled),opened(&engine,&ChartAlertEngine::openWindowRequested);
    engine.clearPrivate();emit sink.actionRequested(SystemNoticeAction::DisableDrip);QVERIFY(!engine.settings().dripEnabled);QCOMPARE(disabled.size(),1);
    emit sink.actionRequested(SystemNoticeAction::OpenWindow);QCOMPARE(opened.size(),1);
}
void ChartAlertEngineTest::liquidationUsesSeriousPlainCurrentRisk(){
    qint64 now=Base;FakeSink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);QVERIFY(holdingRule(engine,now,Future));
    ChartLiquidationObservation position{Future,"synthetic-position-token","100","96","USDT","https://fapi.binance.com/fapi/v3/positionRisk",now,true,"LONG"};
    engine.observeLiquidation(position,engine.privateEpoch());QCOMPARE(sink.posted.size(),1);QVERIFY(sink.posted.first().playSound);QVERIFY(sink.posted.first().privateData);QVERIFY(sink.posted.first().body.contains(QStringLiteral("청산")));
    QCOMPARE(sink.posted.first().direction,MovementDirection::Unknown);QVERIFY(sink.posted.first().fixedRisk);
    const auto serious=sink.posted.first().body;engine.observeLiquidation(position,engine.privateEpoch());QCOMPARE(sink.posted.size(),1);
    position.liquidationPrice="80";engine.observeLiquidation(position,engine.privateEpoch());engine.setDripEnabled(false);position.liquidationPrice="96";engine.observeLiquidation(position,engine.privateEpoch());
    QCOMPARE(sink.posted.size(),2);QCOMPARE(sink.posted.last().body,serious);engine.clearPrivate();QVERIFY(sink.removed.contains(sink.posted.first().identifier));
}
void ChartAlertEngineTest::liquidationPreservesRawProviderSide_data(){
    QTest::addColumn<QString>("side");QTest::addColumn<int>("expected");
    for(const auto* side:{"LONG","SHORT","BOTH","","INFERRED"})QTest::newRow(*side?side:"missing")<<QString(side)<<(QStringList{"LONG","SHORT","BOTH"}.contains(side)?1:0);
}
void ChartAlertEngineTest::liquidationPreservesRawProviderSide(){
    QFETCH(QString,side);QFETCH(int,expected);qint64 now=Base;FakeSink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(holdingRule(engine,now,Future));ChartLiquidationObservation position{Future,"synthetic-position-token","100",side=="SHORT"?"104":"96","USDT","https://fapi.binance.com/fapi/v3/positionRisk",now,true,side};
    engine.observeLiquidation(position,engine.privateEpoch());QCOMPARE(sink.posted.size(),expected);if(expected)QVERIFY(sink.posted.first().body.contains(side));
}
void ChartAlertEngineTest::freshOwnedFuturesRiskNeedsNoSpotBalanceMembership(){
    qint64 now=Base;FakeSink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);engine.setPrivateEnabled(true);
    ChartAlertRule rule;rule.listId="@holdings";rule.enabled=true;rule.liquidation=true;rule.pullback=false;rule.results=false;rule.cooldownMs=0;QVERIFY(engine.setRule(rule));QVERIFY(engine.demands().isEmpty());
    const WatchlistIdentity contract{"usdm_futures","binance","SYNUSDT"};
    QVERIFY(!WatchlistWorkspaceList::validIdentity(contract)); // Risk identity must never enter C2 public/spot lists.
    const ChartLiquidationObservation position{contract,"synthetic-own-futures","100","96","USDT","https://fapi.binance.com/fapi/v3/positionRisk",now,true,"LONG"};
    engine.observeLiquidation(position,engine.privateEpoch());QCOMPARE(sink.posted.size(),1);QVERIFY(sink.posted.first().privateData);QVERIFY(sink.posted.first().body.contains("SYNUSDT"));
    engine.clearPrivate();QVERIFY(sink.removed.contains(sink.posted.first().identifier));
}
void ChartAlertEngineTest::spotIdentityNeverProvesFuturesRiskEvenWhenHeld(){
    const WatchlistIdentity Spot{"coin","binance","SYNUSDT"};
    qint64 now=Base;FakeSink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);QVERIFY(holdingRule(engine,now,Spot));
    QVERIFY(WatchlistWorkspaceList::validIdentity(Spot));
    const ChartLiquidationObservation incorrectlyTyped{Spot,"synthetic-spot-is-not-contract","100","96","USDT","https://fapi.binance.com/fapi/v3/positionRisk",now,true,"LONG"};
    engine.observeLiquidation(incorrectlyTyped,engine.privateEpoch());QVERIFY(sink.posted.isEmpty());
}
void ChartAlertEngineTest::liquidationBatchAcceptsVerifiedOwnedRiskWithoutSpot(){
    qint64 now=Base;FakeSink sink;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);QVERIFY(holdingRule(engine,now,Future));
    const ChartLiquidationObservation position{Future,"synthetic-batch-risk","100","96","USDT","https://fapi.binance.com/fapi/v3/positionRisk",now,true,"LONG"};
    engine.setLiquidationObservations({position},engine.privateEpoch());QCOMPARE(sink.posted.size(),1);QVERIFY(sink.posted.first().privateData);
}
void ChartAlertEngineTest::absentPositionBatchCancelsPendingRiskRetry(){
    qint64 now=Base;FakeSink sink;sink.automaticSuccess=false;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);QVERIFY(holdingRule(engine,now,Future));
    const ChartLiquidationObservation position{Future,"synthetic-closed-risk","100","96","USDT","https://fapi.binance.com/fapi/v3/positionRisk",now,true,"LONG"};
    engine.observeLiquidation(position,engine.privateEpoch());QCOMPARE(sink.posted.size(),1);const auto notice=sink.posted.first();emit sink.submissionFinished(notice.identifier,notice.privateEpoch,false);
    engine.setLiquidationObservations({},engine.privateEpoch());now+=5000;engine.tick();QCOMPARE(sink.posted.size(),1);QVERIFY(sink.removed.contains(notice.identifier));
}
void ChartAlertEngineTest::expiredSpotHoldingsDoNotRevokeFreshFuturesRisk(){
    qint64 now=Base;FakeSink sink;sink.automaticSuccess=false;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(holdingRule(engine,now,Future));const auto epoch=engine.privateEpoch();engine.setHoldings({Coin},now-180000,epoch);
    const ChartLiquidationObservation position{Future,"synthetic-independent-risk","100","96","USDT","https://fapi.binance.com/fapi/v3/positionRisk",now,true,"LONG"};
    engine.setLiquidationObservations({position},epoch);QCOMPARE(sink.posted.size(),1);const auto notice=sink.posted.first();
    emit sink.submissionFinished(notice.identifier,notice.privateEpoch,false);now+=5000;engine.tick();
    QCOMPARE(engine.privateEpoch(),epoch);QVERIFY(engine.demands().isEmpty());QCOMPARE(sink.posted.size(),2);
    QCOMPARE(sink.posted.last().identifier,notice.identifier);QVERIFY(sink.posted.last().body.contains("LONG"));
}
void ChartAlertEngineTest::absentPositionBatchAtRetryDeadlineCannotResubmit(){
    qint64 now=Base;FakeSink sink;sink.automaticSuccess=false;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);QVERIFY(holdingRule(engine,now,Future));
    const ChartLiquidationObservation position{Future,"synthetic-absent-at-deadline","100","96","USDT","https://fapi.binance.com/fapi/v3/positionRisk",now,true,"LONG"};
    const auto epoch=engine.privateEpoch();engine.setLiquidationObservations({position},epoch);QCOMPARE(sink.posted.size(),1);const auto notice=sink.posted.first();
    emit sink.submissionFinished(notice.identifier,notice.privateEpoch,false);now+=5000;engine.setLiquidationObservations({},epoch);
    QCOMPARE(sink.posted.size(),1);QVERIFY(sink.removed.contains(notice.identifier));
}
void ChartAlertEngineTest::onlyPublicConfigurationCanPersist(){
    qint64 now=Base;FakeSink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(publicRule(engine));QVERIFY(holdingRule(engine,now,Coin));
    const auto state=engine.publicState();QVERIFY(!state.isEmpty());QVERIFY(!state.contains("SYN"));QVERIFY(!state.contains("position"));QVERIFY(!state.contains("markPrice"));
    auto document=QJsonDocument::fromJson(state);QVERIFY(document.isObject());auto object=document.object();QVERIFY(object.value("rules").isArray());
    auto settings=object.value("settings").toObject();settings.insert("holdings",QJsonArray{QString("PRIVATE_SENTINEL")});object.insert("settings",settings);
    QVERIFY(!engine.restorePublicState(QJsonDocument(object).toJson()));QCOMPARE(engine.publicState(),state);
    FakeSink restoredSink;ChartAlertEngine restored(&restoredSink,nullptr,[&]{return now;});QVERIFY(restored.setPublicLists({{"public-a",{Coin}}}));
    QVERIFY(restored.restorePublicState(state));QCOMPARE(restored.demands().size(),1);QVERIFY(!restored.demands().first().privateOnly);
}
void ChartAlertEngineTest::publicMarketStateChangePostsOnceWithoutPrivateContext(){
    qint64 now=Base;FakeSink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
    engine.setMarketMetricEnabled(MarketFlowMetric::KoreaGap,true);
    engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Normal,now-2000,now-1000);
    QVERIFY(sink.posted.isEmpty());
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Hot,now-1000,now);
    QCOMPARE(sink.posted.size(),1);QVERIFY(!sink.posted.first().privateData);QCOMPARE(sink.posted.first().privateEpoch,quint64(0));
    QCOMPARE(sink.posted.first().title,QStringLiteral("BTC · 테더 환산 가격차 흐름 변화"));
    QCOMPARE(sink.posted.first().body,QStringLiteral("보통에서 과열 상태로 바뀌었어요"));
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Hot,now-1000,now);
    QCOMPARE(sink.posted.size(),1);QCOMPARE(sink.requests,0);
}
void ChartAlertEngineTest::publicContractFlowChangesUseCanonicalDirections(){
    qint64 now=Base;FakeSink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::OpenInterest,true));
    ContractFlow flow{true,0,0,"PRIVATE_SENTINEL","",now-2000,now-1000};
    engine.observeContractFlow("BTC",flow,now);QVERIFY(sink.posted.isEmpty());
    now+=1000;flow.priceDirection=1;flow.quantityDirection=-1;flow.fromMs=now-2000;flow.toMs=now-1000;
    engine.observeContractFlow("BTC",flow,now);QCOMPARE(sink.posted.size(),1);
    QCOMPARE(sink.posted.first().body,QStringLiteral("가격 보합 · 미결제약정 보합에서 가격 상승 · 미결제약정 하락 상태로 바뀌었어요"));
    QVERIFY(!sink.posted.first().privateData);QCOMPARE(sink.posted.first().privateEpoch,quint64(0));
    QVERIFY(!sink.posted.first().body.contains("PRIVATE_SENTINEL"));
    QVERIFY(!sink.posted.first().body.contains(QStringLiteral("과열")));
    now+=1000;flow.fromMs=now-2000;flow.toMs=now-1000;engine.observeContractFlow("BTC",flow,now);
    engine.observeMarketMetric("BTC",MarketFlowMetric::OpenInterest,MarketFlowState::Hot,now-1000,now);
    QCOMPARE(sink.posted.size(),1);QCOMPARE(sink.requests,0);
}
void ChartAlertEngineTest::publicMarketFlagsPersistSeparatelyAndRejectPrivateFields(){
    qint64 now=Base;FakeSink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
    const auto chartState=engine.publicState();QSignalSpy changed(&engine,&ChartAlertEngine::changed),demand(&engine,&ChartAlertEngine::demandsChanged);
    for(auto metric:{MarketFlowMetric::KoreaGap,MarketFlowMetric::Funding,MarketFlowMetric::CoinbaseGap,MarketFlowMetric::OpenInterest})QVERIFY(!engine.marketMetricEnabled(metric));
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::KoreaGap,true));
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::OpenInterest,true));
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::OpenInterest,true));
    QCOMPARE(changed.size(),2);QCOMPARE(demand.size(),2);QCOMPARE(engine.publicState(),chartState);
    const auto state=engine.marketMetricState();QVERIFY(!state.isEmpty());
    const auto object=QJsonDocument::fromJson(state).object();QCOMPARE(object.size(),5);QCOMPARE(object.value("version").toInt(),1);
    QVERIFY(object.value("koreaGap").toBool());QVERIFY(!object.value("funding").toBool());
    QVERIFY(!object.value("coinbaseGap").toBool());QVERIFY(object.value("openInterest").toBool());
    FakeSink otherSink;ChartAlertEngine restored(&otherSink,nullptr,[&]{return now;});
    QVERIFY(restored.restoreMarketMetricState(state));QCOMPARE(restored.marketMetricState(),state);QCOMPARE(restored.publicState(),chartState);
    for(const auto& key:{"holdings","asset","privateEpoch","sourceTime"}){
        auto invalid=object;invalid.insert(key,"PRIVATE_SENTINEL");QVERIFY(!restored.restoreMarketMetricState(QJsonDocument(invalid).toJson()));
        QCOMPARE(restored.marketMetricState(),state);
    }
    auto invalid=object;invalid["funding"]=1;QVERIFY(!restored.restoreMarketMetricState(QJsonDocument(invalid).toJson()));
    invalid=object;invalid["version"]=2;QVERIFY(!restored.restoreMarketMetricState(QJsonDocument(invalid).toJson()));
    QVERIFY(!restored.restoreMarketMetricState(QByteArray(4097,' ')));QCOMPARE(restored.marketMetricState(),state);
    QVERIFY(!restored.setMarketMetricEnabled(static_cast<MarketFlowMetric>(99),true));
    QCOMPARE(sink.requests+otherSink.requests,0);
}
void ChartAlertEngineTest::publicMarketAssetSwitchCancelsAllPending(){
    qint64 now=Base;FakeSink sink;sink.automaticSuccess=false;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::KoreaGap,true));QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::Funding,true));
    engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Normal,now-2000,now-1000);
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Hot,now-1000,now);
    QCOMPARE(sink.posted.size(),1);const auto notice=sink.posted.first();emit sink.submissionFinished(notice.identifier,0,false);
    engine.observeMarketMetric("ETH",MarketFlowMetric::Funding,MarketFlowState::Normal,now-1000,now);
    QVERIFY(sink.removed.contains(notice.identifier));now+=5000;engine.tick();QCOMPARE(sink.posted.size(),1);
    engine.observeMarketMetric("ETH",MarketFlowMetric::KoreaGap,MarketFlowState::Hot,now-1000,now);QCOMPARE(sink.posted.size(),1);
    now+=1000;engine.observeMarketMetric("ETH",MarketFlowMetric::KoreaGap,MarketFlowState::Cool,now-1000,now);QCOMPARE(sink.posted.size(),2);
}
void ChartAlertEngineTest::publicMarketClocksAndUnavailableStates_data(){
    QTest::addColumn<QString>("fault");QTest::addColumn<int>("expected");
    for(const auto* fault:{"source-reversal","receipt-reversal","stale-source","stale-receipt","future-source","future-receipt","source-after-receipt","equal-clock-conflict","same-state","unavailable","receipt-only","source-only"})
        QTest::newRow(fault)<<QString(fault)<<((QString(fault)=="receipt-only"||QString(fault)=="source-only")?1:0);
}
void ChartAlertEngineTest::publicMarketClocksAndUnavailableStates(){
    QFETCH(QString,fault);QFETCH(int,expected);qint64 now=Base;FakeSink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::Funding,true));
    engine.observeMarketMetric("BTC",MarketFlowMetric::Funding,MarketFlowState::Normal,now-2000,now-1000);
    now+=1000;qint64 source=now-1000,received=now;auto state=MarketFlowState::Hot;
    if(fault=="source-reversal")source=Base-3000;if(fault=="receipt-reversal")received=Base-2000;
    if(fault=="stale-source")source=now-180001;if(fault=="stale-receipt")received=now-180001;
    if(fault=="future-source")source=now+1;if(fault=="future-receipt")received=now+1;
    if(fault=="source-after-receipt"){source=now;received=now-1;}
    if(fault=="equal-clock-conflict"){source=Base-2000;received=Base-1000;}
    if(fault=="same-state")state=MarketFlowState::Normal;if(fault=="unavailable")state=MarketFlowState::Unavailable;
    if(fault=="receipt-only")source=Base-2000;if(fault=="source-only"){source=Base-1000;received=Base-1000;}
    engine.observeMarketMetric("BTC",MarketFlowMetric::Funding,state,source,received);QCOMPARE(sink.posted.size(),expected);
    QCOMPARE(sink.requests,0);
}
void ChartAlertEngineTest::publicContractFlowExpiredWindowCannotRetry(){
    qint64 now=Base;FakeSink sink;sink.automaticSuccess=false;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::OpenInterest,true));
    ContractFlow flow{true,0,0,"","",now-179000,now-60000};engine.observeContractFlow("BTC",flow,now);
    now+=1000;flow.fromMs+=1000;flow.toMs+=1000;flow.priceDirection=1;engine.observeContractFlow("BTC",flow,now);
    QCOMPARE(sink.posted.size(),1);const auto notice=sink.posted.first();emit sink.submissionFinished(notice.identifier,0,false);
    now+=5000;engine.tick();QCOMPARE(sink.posted.size(),1);QVERIFY(sink.removed.contains(notice.identifier));
}
void ChartAlertEngineTest::publicMarketSuppressionDoesNotCatchUp_data(){
    QTest::addColumn<QString>("mode");for(const auto* mode:{"quiet","mute","denied","unknown","unavailable"})QTest::newRow(mode)<<QString(mode);
}
void ChartAlertEngineTest::publicMarketSuppressionDoesNotCatchUp(){
    QFETCH(QString,mode);qint64 now=Base;FakeSink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::CoinbaseGap,true));
    engine.observeMarketMetric("BTC",MarketFlowMetric::CoinbaseGap,MarketFlowState::Normal,now-2000,now-1000);
    auto settings=engine.settings();settings.timeZoneId="UTC";const auto time=QDateTime::fromMSecsSinceEpoch(now,QTimeZone::utc()).time();
    if(mode=="quiet"){settings.quietEnabled=true;settings.quietStartMinute=time.hour()*60+time.minute();settings.quietEndMinute=(settings.quietStartMinute+1)%1440;}
    if(mode=="mute")settings.mutedUntilMs=now+60000;
    if(mode=="denied")sink.permissionValue=SystemNoticePermission::Denied;
    if(mode=="unknown")sink.permissionValue=SystemNoticePermission::Unknown;
    if(mode=="unavailable")sink.permissionValue=SystemNoticePermission::Unavailable;
    QVERIFY(engine.setSettings(settings));now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::CoinbaseGap,MarketFlowState::Hot,now-1000,now);
    QVERIFY(sink.posted.isEmpty());settings.quietEnabled=false;settings.mutedUntilMs=0;QVERIFY(engine.setSettings(settings));sink.permissionValue=SystemNoticePermission::Granted;
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::CoinbaseGap,MarketFlowState::Hot,now-1000,now);engine.tick();QVERIFY(sink.posted.isEmpty());
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::CoinbaseGap,MarketFlowState::Cool,now-1000,now);QCOMPARE(sink.posted.size(),1);
    QCOMPARE(sink.requests,0);
}
void ChartAlertEngineTest::publicMarketSharesChartDailyCapacity(){
    qint64 now=Base+26*Minute;FakeSink sink;sink.automaticSuccess=false;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(publicRule(engine));
    auto settings=engine.settings();settings.dailyLimit=1;QVERIFY(engine.setSettings(settings));QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::KoreaGap,true));
    engine.beginLane(Coin,Minute,1,true);observe(engine,snapshot(26,now));engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Normal,now-2000,now-1000);
    now+=Minute;observe(engine,snapshot(27,now));QCOMPARE(sink.posted.size(),1);
    engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Hot,now-1000,now);QCOMPARE(sink.posted.size(),1);
    const auto chart=sink.posted.first();emit sink.submissionFinished(chart.identifier,chart.privateEpoch,true);
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Cool,now-1000,now);QCOMPARE(sink.posted.size(),1);
    now+=86400000;engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Normal,now-1000,now);QCOMPARE(sink.posted.size(),1);
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Hot,now-1000,now);QCOMPARE(sink.posted.size(),2);
}
void ChartAlertEngineTest::publicMarketRetryRemainsPlainAndPublicAfterLock(){
    qint64 now=Base;FakeSink sink;sink.automaticSuccess=false;ChartPhrasePack phrases;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});engine.setPhrasePack(&phrases);
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::Funding,true));engine.setPrivateEnabled(true);
    engine.observeMarketMetric("BTC",MarketFlowMetric::Funding,MarketFlowState::Normal,now-2000,now-1000);
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::Funding,MarketFlowState::Hot,now-1000,now);QCOMPARE(sink.posted.size(),1);
    const auto notice=sink.posted.first();emit sink.submissionFinished(notice.identifier,0,false);engine.clearPrivate();engine.setDripEnabled(false);
    QVERIFY(!sink.removed.contains(notice.identifier));now+=5000;engine.tick();QCOMPARE(sink.posted.size(),2);
    QCOMPARE(sink.posted.last().identifier,notice.identifier);QCOMPARE(sink.posted.last().title,notice.title);QCOMPARE(sink.posted.last().body,QStringLiteral("보통에서 과열 상태로 바뀌었어요"));
    QVERIFY(!sink.posted.last().privateData);QCOMPARE(sink.posted.last().privateEpoch,quint64(0));QCOMPARE(sink.posted.last().direction,MovementDirection::Unknown);QVERIFY(!sink.posted.last().fixedRisk);
    emit sink.submissionFinished(notice.identifier,0,false);now+=5000;engine.tick();QCOMPARE(sink.posted.size(),2);QCOMPARE(sink.requests,0);
}
void ChartAlertEngineTest::publicMarketInvalidationAtRetryDeadlineCancels_data(){
    QTest::addColumn<QString>("mode");for(const auto* mode:{"disable","unavailable","clock-conflict"})QTest::newRow(mode)<<QString(mode);
}
void ChartAlertEngineTest::publicMarketInvalidationAtRetryDeadlineCancels(){
    QFETCH(QString,mode);qint64 now=Base;FakeSink sink;sink.automaticSuccess=false;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::KoreaGap,true));engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Normal,now-2000,now-1000);
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Hot,now-1000,now);QCOMPARE(sink.posted.size(),1);
    const auto notice=sink.posted.first();const auto source=now-1000,received=now;emit sink.submissionFinished(notice.identifier,0,false);now+=5000;
    if(mode=="disable")QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::KoreaGap,false));
    else engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,mode=="unavailable"?MarketFlowState::Unavailable:MarketFlowState::Cool,source,received);
    engine.tick();QCOMPARE(sink.posted.size(),1);QVERIFY(sink.removed.contains(notice.identifier));
    emit sink.submissionFinished(notice.identifier,0,true);engine.tick();QCOMPARE(sink.posted.size(),1);
    QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::KoreaGap,true));now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::KoreaGap,MarketFlowState::Hot,now-1000,now);QCOMPARE(sink.posted.size(),1);
}
void ChartAlertEngineTest::publicMarketUnavailableGapRebaselines(){
    qint64 now=Base;FakeSink sink;ChartAlertEngine engine(&sink,nullptr,[&]{return now;});QVERIFY(engine.setMarketMetricEnabled(MarketFlowMetric::Funding,true));
    engine.observeMarketMetric("BTC",MarketFlowMetric::Funding,MarketFlowState::Normal,now-2000,now-1000);
    now+=180001;engine.observeMarketMetric("BTC",MarketFlowMetric::Funding,MarketFlowState::Hot,now-1000,now);QVERIFY(sink.posted.isEmpty());
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::Funding,MarketFlowState::Unavailable,now-1000,now);
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::Funding,MarketFlowState::Cool,now-1000,now);QVERIFY(sink.posted.isEmpty());
    now+=1000;engine.observeMarketMetric("BTC",MarketFlowMetric::Funding,MarketFlowState::Normal,now-1000,now);QCOMPARE(sink.posted.size(),1);
}
QTEST_GUILESS_MAIN(ChartAlertEngineTest)
#include "tst_chart_alert_engine.moc"
