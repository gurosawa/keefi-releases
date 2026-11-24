#include "keefi/CoinFlowStudies.h"
#include "keefi/ExchangeIdentifiers.h"
#include <QTest>
#include <algorithm>
#include <cmath>
using namespace keefi;

namespace {
constexpr qint64 Minute=60000, Start=1792713600000; // Synthetic UTC-aligned time.
CoinFlowEvidence evidence(qint64 known) {
    return {"합성 공개 원천", "https://example.invalid/synthetic-only", 0, known, known};
}
CoinFlowInstrument instrument(CoinFlowVenue venue=CoinFlowVenue::BinanceSpot) {
    CoinFlowInstrument result;
    result.venue=venue;
    result.grain=venue==CoinFlowVenue::BinanceUsdM ? CoinFlowGrain::Futures100msAggregate
        : venue==CoinFlowVenue::UpbitSpot ? CoinFlowGrain::UpbitTrade : CoinFlowGrain::SpotAggregate;
    result.identity=venue==CoinFlowVenue::UpbitSpot ? "coin:upbit:KRW-SYN"
        : venue==CoinFlowVenue::BinanceSpot ? "coin:binance:SYNUSDT" : "usdm_futures:binance:SYNUSDT";
    result.symbol=venue==CoinFlowVenue::UpbitSpot ? "KRW-SYN" : "SYNUSDT";
    result.baseAsset="SYN";
    result.quoteAsset=venue==CoinFlowVenue::UpbitSpot ? "KRW" : "USDT";
    result.status="TRADING";
    if (venue==CoinFlowVenue::BinanceUsdM) {
        result.marginAsset="USDT"; result.contractType="PERPETUAL"; result.underlyingType="COIN";
    }
    result.verified=true; result.evidence=evidence(Start-1);
    result.evidence.sourceUrl=venue==CoinFlowVenue::UpbitSpot ? "https://api.upbit.com/v1/market/all?is_details=true"
        :venue==CoinFlowVenue::BinanceSpot ? "https://api.binance.com/api/v3/exchangeInfo?permissions=SPOT&showPermissionSets=false"
        :"https://fapi.binance.com/fapi/v1/exchangeInfo";
    return result;
}
CoinFlowClosedMinute minute(qint64 start, QString close="100", QString volume="10") {
    CoinFlowClosedMinute result;
    result.candle={start,0,close,close,close,close,volume};
    result.completesAtMs=start+Minute;
    result.knownAtMs=result.decisionAtMs=start+Minute+1000;
    result.connectsPrevious=true; result.liveEligible=true;
    result.evidence=evidence(result.knownAtMs);
    return result;
}
CoinFlowSnapshot snapshot(int count=1) {
    CoinFlowSnapshot result; result.primary.instrument=instrument();
    result.identity=result.primary.instrument.identity;
    for (int i=0;i<count;++i) result.minutes.append(minute(Start+i*Minute));
    result.minutes.first().connectsPrevious=false;
    result.primary.evidence=evidence(result.minutes.last().knownAtMs);
    return result;
}
void observed(CoinFlowWindow& window, qint64 from, qint64 through, quint64 epoch=1) {
    window.periods.append({from,through,through+1,epoch,CoinFlowCoverage::ObservedContinuous,{}});
}
CoinTradeObservation trade(int id,qint64 at,QString quantity="10",CoinFlowSide side=CoinFlowSide::Buy) {
    CoinTradeObservation result;
    result.id=QString::number(id); result.price="10"; result.quantity=quantity;
    result.side=side; result.tradeAtMs=at; result.eventAtMs=at+1;
    result.receivedAtMs=result.knownAtMs=at+2; result.epoch=1; result.liveEligible=true;
    return result;
}
bool hasKind(const CoinFlowStudySet& result, CoinFlowSignalKind kind) {
    for (const auto& entry:result.entries) if (entry.kind==kind) return true;
    return false;
}
MarketCandleSeries candleProof(CoinFlowSnapshot& input) {
    MarketCandleSeries result; result.available=true; result.identity=input.identity;
    result.asset="SYN"; result.currency="USDT"; result.intervalMs=Minute;
    result.sourceUrl=MarketCandles::request(input.identity,Minute).url.toString();
    result.receivedAtMs=input.minutes.last().knownAtMs;
    for(auto& minute:input.minutes) {
        result.candles.append(minute.candle);
        minute.evidence.sourceUrl=result.sourceUrl;
    }
    return result;
}
}

class CoinFlowStudiesTest final:public QObject {
    Q_OBJECT
  private slots:
    void dev86DisplayPhasesRequireActualCompleteObservation();
    void dev86MarkersUseContainingNativeCandlesAndNeverFillGaps();
    void volumeAndReversalDoNotDependOnMissingTradeOrOiSources();
    void percentileUsesPrevious200AndExcludesCandidate();
    void equalNotionalPopulationIsMidrank50();
    void takerUsesQuoteNotionalAndActual30SampleBoundary();
    void upbitDirectionNeverBecomesDocumentedTaker();
    void cvdUsesBaseQuantityAndNeverJoinsEpochs();
    void oiRequiresExactlyAdjacentEndsAndOriginalKnownAt();
    void historicalSeedCannotMintOldLiveSignals();
    void suffixCannotChangePastSignalEntries();
    void prefixTrimCannotChangeLaterSignalEntries();
    void groupingStartsOnceAndPhysicalOneHourOutcomeNeedsExactTarget();
    void incompleteCompanionOnlyHidesComparison();
    void observedLiquidationPreservesRawSideAndLossySampling();
    void strictTradeLexemesDoNotPoisonIndependentCandleStudies_data();
    void strictTradeLexemesDoNotPoisonIndependentCandleStudies();
    void invalidCandlePartDoesNotEraseObservedPublicFlow();
    void knownAtAndClosedMinuteBoundaryGateActualInputs_data();
    void knownAtAndClosedMinuteBoundaryGateActualInputs();
    void sourceBoundsFailOnlyTheAffectedIndependentPart_data();
    void sourceBoundsFailOnlyTheAffectedIndependentPart();
    void zeroDenominatorsRemainUnavailableRatherThanZeroSignals();
    void comparisonKeepsVerifiedPopulationsSeparateOnTheirCommonWindow();
    void partialLiquidationCoverageIsAnObservedListRatherThanMarketTotal();
    void latestPartialCompanionCannotFallBackToAnOlderCompleteComparison();
    void independentCanonicalCandlesSurviveMissingFlowMetadata();
    void forgedSpotMetadataCannotRelabelOrEnableFlow();
    void invalidCatalogProvenanceHidesFlowOnly_data();
    void invalidCatalogProvenanceHidesFlowOnly();
    void verifiedAstralBaseAtOfficialScalarBoundaryKeepsObservedFlow();
    void reversalCorroborationUsesOriginalDecisionOnly();
};

void CoinFlowStudiesTest::dev86DisplayPhasesRequireActualCompleteObservation(){
    auto input=snapshot(21);const auto last=Start+20*Minute,through=last+Minute;
    const auto missing=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QCOMPARE(coinFlowDisplayState(missing).phase,CoinFlowDisplayPhase::Collecting);
    observed(input.primary,last,through);
    for(int i=0;i<201;++i)input.primary.trades.append(trade(i+1,last+100+i*200,"10",i%2?CoinFlowSide::Sell:CoinFlowSide::Buy));
    const auto observed=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(observed.largeTrade.state.available);QVERIFY(observed.taker.state.available);QVERIFY(observed.volume.state.available);QVERIFY(observed.reversal.state.available);
    const auto none=coinFlowDisplayState(observed);QVERIFY(none.requiredReady);
    QCOMPARE(none.phase,CoinFlowDisplayPhase::ObservedNoSignal);QCOMPARE(none.currentSignals,0);QCOMPARE(none.latestSignalAtMs,qint64(0));
    input.minutes.last().candle.volume="40";
    const auto signalled=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    const auto signal=coinFlowDisplayState(signalled);QCOMPARE(signal.phase,CoinFlowDisplayPhase::Signal);QCOMPARE(signal.latestSignalAtMs,through);QVERIFY(signal.currentSignals>0);
    // An absent trade history is not zero flow, even though candles exist.
    input.minutes.last().candle.volume="10";input.primary.trades.clear();
    const auto empty=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QCOMPARE(coinFlowDisplayState(empty).phase,CoinFlowDisplayPhase::Collecting);
    auto stale=observed;stale.asOfMs=stale.lastCompletedAtMs+3*Minute;
    QCOMPARE(coinFlowDisplayState(stale).phase,CoinFlowDisplayPhase::Collecting);
    auto future=observed;CoinFlowSignal entry;entry.state.available=true;entry.atMs=through;entry.knownAtMs=future.asOfMs+1;future.entries.append(entry);
    QCOMPARE(coinFlowDisplayState(future).phase,CoinFlowDisplayPhase::ObservedNoSignal);
    // Optional OI stays non-required, but a real displayed OI group must not
    // contradict the summary by being called no-signal.
    future.entries.last().kind=CoinFlowSignalKind::OpenInterestJump;future.entries.last().knownAtMs=future.asOfMs;
    QCOMPARE(coinFlowDisplayState(future).phase,CoinFlowDisplayPhase::Signal);
}
void CoinFlowStudiesTest::dev86MarkersUseContainingNativeCandlesAndNeverFillGaps(){
    QList<MarketCandle> rows;
    rows.append({Start,0,"100","102","98","101","10"});
    rows.append({Start+30*Minute,0,"101","103","99","102","10"});
    QCOMPARE(coinFlowMarkerCandleAt(Start+Minute,rows,15*Minute),std::optional<qint64>(Start));
    QCOMPARE(coinFlowMarkerCandleAt(Start+15*Minute,rows,15*Minute),std::optional<qint64>(Start));
    QVERIFY(!coinFlowMarkerCandleAt(Start+16*Minute,rows,15*Minute));
    QCOMPARE(coinFlowMarkerCandleAt(Start+31*Minute,rows,15*Minute),std::optional<qint64>(Start+30*Minute));
    QVERIFY(!coinFlowMarkerCandleAt(Start,rows,15*Minute));
    QVERIFY(!coinFlowMarkerCandleAt(Start+46*Minute,rows,15*Minute));
}
void CoinFlowStudiesTest::reversalCorroborationUsesOriginalDecisionOnly(){
    auto input=snapshot(90);
    const QStringList closes{"100","96","94","95"};
    for(int i=0;i<4;++i){auto& c=input.minutes[17+i].candle;c.open=c.high=c.low=c.close=closes[i];}
    input.minutes[20].candle.open=input.minutes[20].candle.low="94";input.minutes[20].candle.volume="20";
    for(const auto& row:input.minutes)observed(input.primary,Start,row.completesAtMs);
    input.primary.trades.append(trade(1,Start+20*Minute+10,"10"));
    const auto decision=input.minutes[20].decisionAtMs;
    const auto findReversal=[](const CoinFlowStudySet& result){
        for(const auto& entry:result.entries)if(entry.kind==CoinFlowSignalKind::Rebound&&entry.atMs==Start+21*Minute)return entry;
        return CoinFlowSignal{};
    };
    QVERIFY(!findReversal(coinFlowStudies(input,decision)).reversalCorroboration);
    const auto first=findReversal(coinFlowStudiesWithReversalEvidence(input,decision));
    QVERIFY(first.reversalCorroboration);QCOMPARE(first.knownAtMs,decision);
    QVERIFY(first.reversalCorroboration->spot.state.available);
    QCOMPARE(first.reversalCorroboration->spot.signedFraction,std::optional<double>(1));
    QCOMPARE(first.reversalCorroboration->volume.multiple,std::optional<double>(2));
    auto late=trade(2,Start+20*Minute+20,"10000",CoinFlowSide::Sell);
    late.receivedAtMs=late.knownAtMs=decision+Minute;input.primary.trades.append(late);
    input.minutes[50].candle.volume="100000";
    const auto asOf=input.minutes.last().decisionAtMs;
    const auto after=findReversal(coinFlowStudiesWithReversalEvidence(input,asOf));
    QVERIFY(after.reversalCorroboration);
    QCOMPARE(after.reversalCorroboration->spot.signedFraction,first.reversalCorroboration->spot.signedFraction);
    QCOMPARE(after.reversalCorroboration->volume.multiple,first.reversalCorroboration->volume.multiple);
    // If the original evidence was absent, receiving it later cannot supply an old grade.
    input.primary.trades.first().receivedAtMs=input.primary.trades.first().knownAtMs=decision+Minute;
    const auto missing=findReversal(coinFlowStudiesWithReversalEvidence(input,asOf));
    QVERIFY(missing.reversalCorroboration);QVERIFY(!missing.reversalCorroboration->spot.state.available);
    input.primary.trades.first().receivedAtMs=input.primary.trades.first().knownAtMs=Start+20*Minute+12;
    input.primary.periods[20].knownAtMs=decision+Minute;
    const auto lateCoverage=findReversal(coinFlowStudiesWithReversalEvidence(input,asOf));
    QVERIFY(lateCoverage.reversalCorroboration);QVERIFY(!lateCoverage.reversalCorroboration->spot.state.available);
}

void CoinFlowStudiesTest::volumeAndReversalDoNotDependOnMissingTradeOrOiSources() {
    auto input=snapshot(21);
    input.minutes.last().candle.volume="40";
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.volume.state.available); QVERIFY(result.volume.multiple.has_value());
    QCOMPARE(*result.volume.multiple,4.0);
    QCOMPARE(result.volume.previousMedian,QString("10"));
    QVERIFY(!result.openInterest.state.available); QVERIFY(!result.taker.state.available);
    QVERIFY(!result.cvd.state.available); QVERIFY(hasKind(result,CoinFlowSignalKind::VolumeBurst));
    auto reversal=snapshot(4);
    const QString closes[]={"100","96","94","95"};
    for(int i=0;i<4;++i) reversal.minutes[i].candle={Start+i*Minute,0,closes[i],closes[i],closes[i],closes[i],"10"};
    reversal.minutes.last().candle.open="94";
    reversal.minutes.last().candle.low="94";
    const auto bounce=coinFlowStudies(reversal,reversal.minutes.last().decisionAtMs);
    QVERIFY(bounce.reversal.state.available); QVERIFY(bounce.reversal.rebound);
    QVERIFY(!bounce.reversal.turnDown); QVERIFY(hasKind(bounce,CoinFlowSignalKind::Rebound));
}
void CoinFlowStudiesTest::percentileUsesPrevious200AndExcludesCandidate() {
    auto input=snapshot(); observed(input.primary,Start,Start+Minute);
    for(int i=0;i<200;++i) input.primary.trades.append(trade(i+1,Start+100+i*100));
    input.primary.trades.append(trade(201,Start+Minute-1000,"100"));
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.largeTrade.state.available); QVERIFY(result.largeTrade.latestPercentile.has_value());
    QCOMPARE(*result.largeTrade.latestPercentile,100.0);
    QCOMPARE(result.largeTrade.baselineSamples,200); QCOMPARE(result.largeTrade.candidates.size(),1);
    QCOMPARE(result.largeTrade.candidates.first().id,QString("201"));
    input.primary.trades.removeFirst();
    const auto shortWindow=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(!shortWindow.largeTrade.state.available); QVERIFY(shortWindow.largeTrade.candidates.isEmpty());
}
void CoinFlowStudiesTest::equalNotionalPopulationIsMidrank50() {
    auto input=snapshot(); observed(input.primary,Start,Start+Minute);
    for(int i=0;i<201;++i) input.primary.trades.append(trade(i+1,Start+100+i*100));
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.largeTrade.state.available); QVERIFY(result.largeTrade.latestPercentile.has_value());
    QCOMPARE(*result.largeTrade.latestPercentile,50.0); QVERIFY(result.largeTrade.candidates.isEmpty());
}
void CoinFlowStudiesTest::takerUsesQuoteNotionalAndActual30SampleBoundary() {
    auto input=snapshot(); observed(input.primary,Start,Start+Minute);
    for(int i=0;i<30;++i) input.primary.trades.append(trade(i+1,Start+100+i*100,"1",
        i<20 ? CoinFlowSide::Buy : CoinFlowSide::Sell));
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.taker.state.available); QCOMPARE(result.taker.state.samples,30);
    QVERIFY(result.taker.signedFraction.has_value());
    QVERIFY(std::abs(*result.taker.signedFraction-1.0/3.0)<1e-12);
    QVERIFY(hasKind(result,CoinFlowSignalKind::TakerSkew));
    input.primary.trades.removeLast();
    const auto fewer=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(!hasKind(fewer,CoinFlowSignalKind::TakerSkew));
}
void CoinFlowStudiesTest::upbitDirectionNeverBecomesDocumentedTaker() {
    auto input=snapshot(); input.primary.instrument=instrument(CoinFlowVenue::UpbitSpot);
    input.identity=input.primary.instrument.identity; observed(input.primary,Start,Start+Minute);
    for(int i=0;i<30;++i) input.primary.trades.append(trade(i+1,Start+100+i*100,"1"));
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.taker.sourceDirectionOnly); QVERIFY(result.cvd.sourceDirectionOnly);
    QVERIFY(!hasKind(result,CoinFlowSignalKind::TakerSkew));
}
void CoinFlowStudiesTest::cvdUsesBaseQuantityAndNeverJoinsEpochs() {
    auto input=snapshot(2); observed(input.primary,Start,Start+Minute);
    input.primary.trades={trade(1,Start+100,"2"),trade(2,Start+200,"3",CoinFlowSide::Sell)};
    auto next=trade(3,Start+Minute+100,"4"); next.epoch=2;
    input.primary.trades.append(next); observed(input.primary,Start+Minute,Start+2*Minute,2);
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.cvd.state.available); QVERIFY(!result.cvd.points.isEmpty());
    QCOMPARE(result.cvd.points.last().epoch,quint64(2));
    QCOMPARE(result.cvd.points.last().signedBaseQuantity,QString("4"));
    bool earlier=false;
    for(const auto& point:result.cvd.points) if(point.epoch==1 && point.signedBaseQuantity=="-1") earlier=true;
    QVERIFY(earlier);
}
void CoinFlowStudiesTest::oiRequiresExactlyAdjacentEndsAndOriginalKnownAt() {
    auto input=snapshot(11); const qint64 end=Start+10*Minute;
    input.companion=CoinFlowWindow{}; input.companion->instrument=instrument(CoinFlowVenue::BinanceUsdM);
    CoinOiPoint before{"SYNUSDT","100","1000",end-5*Minute,end-5*Minute+10,true,evidence(end-5*Minute+10)};
    CoinOiPoint current{"SYNUSDT","102","1100",end,end+10,true,evidence(end+10)};
    input.openInterest={before,current};
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.openInterest.state.available); QVERIFY(result.openInterest.quantityChangePercent.has_value());
    QVERIFY(std::abs(*result.openInterest.quantityChangePercent-2)<1e-12);
    current.periodEndMs+=5*Minute; input.openInterest={before,current};
    const auto gap=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(!gap.openInterest.state.available);
    current.periodEndMs=end; current.knownAtMs=input.minutes.last().decisionAtMs+1; input.openInterest={before,current};
    const auto late=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(!hasKind(late,CoinFlowSignalKind::OpenInterestJump));
}
void CoinFlowStudiesTest::historicalSeedCannotMintOldLiveSignals() {
    auto input=snapshot(21); input.minutes.last().candle.volume="40";
    for(auto& row:input.minutes) row.liveEligible=false;
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.volume.state.available); QVERIFY(result.entries.isEmpty()); QVERIFY(result.groups.isEmpty());
}
void CoinFlowStudiesTest::suffixCannotChangePastSignalEntries() {
    auto prefix=snapshot(21); prefix.minutes.last().candle.volume="40";
    const auto first=coinFlowStudies(prefix,prefix.minutes.last().decisionAtMs);
    QVERIFY(hasKind(first,CoinFlowSignalKind::VolumeBurst));
    auto suffix=prefix;
    for(int i=21;i<30;++i) suffix.minutes.append(minute(Start+i*Minute,"200","999"));
    // A late OI payload describing a past period is not present at past decisions.
    suffix.openInterest={{"SYNUSDT","100","100",Start+5*Minute,Start+29*Minute,true,evidence(Start+29*Minute)},
        {"SYNUSDT","200","200",Start+10*Minute,Start+29*Minute,true,evidence(Start+29*Minute)}};
    const auto after=coinFlowStudies(suffix,suffix.minutes.last().decisionAtMs);
    int past=0;
    for(const auto& item:after.entries) if(item.atMs<=prefix.minutes.last().completesAtMs) {
        QVERIFY(item.kind==CoinFlowSignalKind::VolumeBurst);
        QCOMPARE(item.atMs,first.entries.first().atMs); QCOMPARE(item.knownAtMs,first.entries.first().knownAtMs);
        QCOMPARE(item.state.samples,first.entries.first().state.samples); ++past;
    }
    QCOMPARE(past,first.entries.size());
}
void CoinFlowStudiesTest::groupingStartsOnceAndPhysicalOneHourOutcomeNeedsExactTarget() {
    auto input=snapshot(83);
    input.minutes[20].candle.volume="40"; input.minutes[21].candle.volume="40";
    input.minutes[80].candle={Start+80*Minute,0,"110","110","110","110","10"};
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    const auto group=std::find_if(result.groups.begin(),result.groups.end(),[](const auto& item){return item.kind==CoinFlowSignalKind::VolumeBurst;});
    QVERIFY(group!=result.groups.end()); QCOMPARE(group->fromMs,Start+21*Minute);
    QCOMPARE(group->throughMs,Start+22*Minute); QCOMPARE(group->minutes,2); QVERIFY(group->ended);
    QCOMPARE(result.scorecard.horizonMs,qint64(60*Minute));
    bool exact=false;
    for(const auto& outcome:result.scorecard.outcomes) if(outcome.kind==CoinFlowSignalKind::VolumeBurst && outcome.signalAtMs==Start+21*Minute) {
        QCOMPARE(outcome.evaluationAtMs,Start+81*Minute);
        QVERIFY(std::abs(outcome.returnPercent-10)<1e-12); exact=true;
    }
    QVERIFY(exact);
    input.minutes.removeAt(80); input.minutes[80].connectsPrevious=false;
    const auto missing=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    for(const auto& outcome:missing.scorecard.outcomes) QVERIFY(outcome.signalAtMs!=Start+21*Minute);
    QVERIFY(missing.scorecard.unresolvedSignals>=1);
}
void CoinFlowStudiesTest::incompleteCompanionOnlyHidesComparison() {
    auto input=snapshot(21); input.minutes.last().candle.volume="40";
    observed(input.primary,Start,Start+21*Minute);
    input.primary.trades={trade(1,Start+20*Minute+100,"2")};
    input.companion=CoinFlowWindow{}; input.companion->instrument=instrument(CoinFlowVenue::BinanceUsdM);
    input.companion->periods.append({Start+20*Minute,Start+21*Minute,Start+21*Minute+1,1,CoinFlowCoverage::Partial,"합성 누락"});
    input.companion->trades={trade(2,Start+20*Minute+200,"3")};
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.volume.state.available); QVERIFY(!result.comparison.state.available);
    QVERIFY(!result.comparison.state.reason.isEmpty());
}
void CoinFlowStudiesTest::observedLiquidationPreservesRawSideAndLossySampling() {
    auto input=snapshot(); input.companion=CoinFlowWindow{};
    input.companion->instrument=instrument(CoinFlowVenue::BinanceUsdM);
    CoinLiquidationObservation item;
    item.symbol="SYNUSDT"; item.side="SELL"; item.price="100"; item.averagePrice="99";
    item.originalQuantity="5"; item.lastFilledQuantity="1"; item.cumulativeFilledQuantity="2";
    item.orderStatus="PARTIALLY_FILLED"; item.tradeAtMs=Start+1000; item.eventAtMs=Start+1001;
    item.receivedAtMs=item.knownAtMs=Start+1002; item.epoch=1; item.evidence=evidence(item.knownAtMs);
    input.liquidations={item};
    input.liquidationCoverage={{Start,Start+Minute,Start+Minute+1,1,CoinFlowCoverage::ObservedContinuous,{}}};
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.liquidation.state.available); QVERIFY(result.liquidation.sampledLastPerSecond);
    QCOMPARE(result.liquidation.observations.size(),1);
    QCOMPARE(result.liquidation.observations.first().side,QString("SELL"));
    QCOMPARE(result.liquidation.observations.first().cumulativeFilledQuantity,QString("2"));
    QCOMPARE(result.liquidation.observations.first().eventAtMs,item.eventAtMs);
    QCOMPARE(result.liquidation.state.samples,1);
}

void CoinFlowStudiesTest::strictTradeLexemesDoNotPoisonIndependentCandleStudies_data() {
    QTest::addColumn<QString>("field"); QTest::addColumn<QString>("value");
    QTest::newRow("price-LF") << QString("price") << QString("10\n");
    QTest::newRow("price-space") << QString("price") << QString(" 10");
    QTest::newRow("price-exponent") << QString("price") << QString("1e1");
    QTest::newRow("price-leading-zero") << QString("price") << QString("010");
    QTest::newRow("price-zero") << QString("price") << QString("0");
    QTest::newRow("price-over-bound") << QString("price") << QString(102,QLatin1Char('9'));
    QTest::newRow("quantity-negative") << QString("quantity") << QString("-1");
    QTest::newRow("quantity-zero") << QString("quantity") << QString("0");
    QTest::newRow("quantity-nan") << QString("quantity") << QString("NaN");
    QTest::newRow("quantity-LF") << QString("quantity") << QString("1\n");
    QTest::newRow("opaque-id-not-integer") << QString("id") << QString("1.0");
    QTest::newRow("opaque-id-LF") << QString("id") << QString("201\n");
}
void CoinFlowStudiesTest::strictTradeLexemesDoNotPoisonIndependentCandleStudies() {
    QFETCH(QString,field); QFETCH(QString,value);
    auto input=snapshot(21); input.minutes.last().candle.volume="40";
    observed(input.primary,Start+20*Minute,Start+21*Minute);
    for(int i=0;i<201;++i) input.primary.trades.append(trade(i+1,Start+20*Minute+100+i*100));
    const auto valid=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(valid.largeTrade.state.available); QVERIFY(valid.volume.state.available);
    auto& item=input.primary.trades.last();
    if(field=="price") item.price=value;
    if(field=="quantity") item.quantity=value;
    if(field=="id") item.id=value;
    const auto invalid=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(!invalid.largeTrade.state.available); QVERIFY(invalid.largeTrade.candidates.isEmpty());
    QVERIFY(!invalid.largeTrade.state.reason.isEmpty());
    QVERIFY(invalid.volume.state.available); QCOMPARE(*invalid.volume.multiple,4.0);
    QVERIFY(hasKind(invalid,CoinFlowSignalKind::VolumeBurst));
    QVERIFY(!hasKind(invalid,CoinFlowSignalKind::LargeTrade));
}
void CoinFlowStudiesTest::invalidCandlePartDoesNotEraseObservedPublicFlow() {
    auto input=snapshot(21); input.minutes.last().candle.volume="40\n";
    observed(input.primary,Start+20*Minute,Start+21*Minute);
    input.primary.trades={trade(1,Start+20*Minute+100,"2"),trade(2,Start+20*Minute+200,"1",CoinFlowSide::Sell)};
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.cvd.state.available); QCOMPARE(result.cvd.points.last().signedBaseQuantity,QString("1"));
    QVERIFY(!result.volume.state.available); QVERIFY(!result.volume.multiple.has_value());
    QVERIFY(!hasKind(result,CoinFlowSignalKind::VolumeBurst));
}
void CoinFlowStudiesTest::knownAtAndClosedMinuteBoundaryGateActualInputs_data() {
    QTest::addColumn<QString>("fault");
    for(const auto* name:{"late-known","next-minute","future-asof","decision-before-close","coverage-known-late"})
        QTest::newRow(name)<<QString(name);
}
void CoinFlowStudiesTest::knownAtAndClosedMinuteBoundaryGateActualInputs() {
    QFETCH(QString,fault);
    auto input=snapshot(); observed(input.primary,Start,Start+Minute);
    for(int i=0;i<30;++i) input.primary.trades.append(trade(i+1,Start+100+i*100,"1"));
    const qint64 decision=input.minutes.last().decisionAtMs;
    const auto baseline=coinFlowStudies(input,decision);
    QVERIFY(hasKind(baseline,CoinFlowSignalKind::TakerSkew));
    qint64 asOf=decision;
    if(fault=="late-known") input.primary.trades.last().receivedAtMs=input.primary.trades.last().knownAtMs=decision+1;
    if(fault=="next-minute") {
        auto& item=input.primary.trades.last(); item.tradeAtMs=Start+Minute;
        item.eventAtMs=item.tradeAtMs+1; item.receivedAtMs=item.knownAtMs=item.tradeAtMs+2;
    }
    if(fault=="future-asof") asOf=Start+Minute-1;
    if(fault=="decision-before-close") input.minutes.last().decisionAtMs=Start+Minute-1;
    if(fault=="coverage-known-late") input.primary.periods.last().knownAtMs=decision+1;
    const auto result=coinFlowStudies(input,asOf);
    QVERIFY(!hasKind(result,CoinFlowSignalKind::TakerSkew));
    if(fault=="late-known"||fault=="next-minute") QCOMPARE(result.taker.state.samples,29);
}
void CoinFlowStudiesTest::sourceBoundsFailOnlyTheAffectedIndependentPart_data() {
    QTest::addColumn<QString>("part");
    QTest::newRow("trades-10001")<<QString("trades");
    QTest::newRow("oi-501")<<QString("oi");
    QTest::newRow("liquidations-1001")<<QString("liquidations");
}
void CoinFlowStudiesTest::sourceBoundsFailOnlyTheAffectedIndependentPart() {
    QFETCH(QString,part);
    auto input=snapshot(21); input.minutes.last().candle.volume="40";
    observed(input.primary,Start+20*Minute,Start+21*Minute);
    input.companion=CoinFlowWindow{}; input.companion->instrument=instrument(CoinFlowVenue::BinanceUsdM);
    input.liquidationCoverage={{Start+20*Minute,Start+21*Minute,Start+21*Minute+1,1,CoinFlowCoverage::ObservedContinuous,{}}};
    if(part=="trades") for(int i=0;i<10001;++i) input.primary.trades.append(trade(i+1,Start+20*Minute+i,"1"));
    if(part=="oi") for(int i=0;i<501;++i) input.openInterest.append({"SYNUSDT","1","1",Start-(500-i)*5*Minute,Start+21*Minute,true,evidence(Start+21*Minute)});
    if(part=="liquidations") for(int i=0;i<1001;++i) {
        CoinLiquidationObservation item; item.symbol="SYNUSDT"; item.side="SELL";
        item.price=item.averagePrice="100"; item.originalQuantity=item.lastFilledQuantity=item.cumulativeFilledQuantity="1";
        item.orderStatus="FILLED"; item.tradeAtMs=Start+20*Minute+i;
        item.eventAtMs=item.tradeAtMs+1; item.receivedAtMs=item.knownAtMs=item.tradeAtMs+2;
        item.epoch=1; item.evidence=evidence(item.knownAtMs); input.liquidations.append(item);
    }
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.volume.state.available); QVERIFY(hasKind(result,CoinFlowSignalKind::VolumeBurst));
    const auto& state=part=="trades" ? result.largeTrade.state : part=="oi" ? result.openInterest.state : result.liquidation.state;
    QVERIFY(!state.available); QCOMPARE(state.coverage,CoinFlowCoverage::Overflow);
    QVERIFY(!state.reason.isEmpty());
}
void CoinFlowStudiesTest::zeroDenominatorsRemainUnavailableRatherThanZeroSignals() {
    auto input=snapshot(21);
    for(auto& item:input.minutes) item.candle.volume="0";
    input.companion=CoinFlowWindow{}; input.companion->instrument=instrument(CoinFlowVenue::BinanceUsdM);
    input.openInterest={{"SYNUSDT","0","0",Start+15*Minute,Start+15*Minute+10,true,evidence(Start+15*Minute+10)},
        {"SYNUSDT","10","100",Start+20*Minute,Start+20*Minute+10,true,evidence(Start+20*Minute+10)}};
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(!result.volume.state.available); QVERIFY(!result.volume.multiple.has_value());
    QVERIFY(!result.openInterest.state.available); QVERIFY(!result.openInterest.quantityChangePercent.has_value());
    QVERIFY(!hasKind(result,CoinFlowSignalKind::VolumeBurst));
    QVERIFY(!hasKind(result,CoinFlowSignalKind::OpenInterestJump));
}
void CoinFlowStudiesTest::comparisonKeepsVerifiedPopulationsSeparateOnTheirCommonWindow() {
    auto input=snapshot(2);
    observed(input.primary,Start+Minute,Start+2*Minute);
    input.companion=CoinFlowWindow{}; input.companion->instrument=instrument(CoinFlowVenue::BinanceUsdM);
    observed(*input.companion,Start,Start+2*Minute,7);
    for(int i=0;i<30;++i) {
        input.primary.trades.append(trade(i+1,Start+Minute+100+i*100,"1",i<20 ? CoinFlowSide::Buy : CoinFlowSide::Sell));
        auto future=trade(i+1,Start+Minute+200+i*100,"1",i<10 ? CoinFlowSide::Buy : CoinFlowSide::Sell);
        future.epoch=7; future.normalQuantity="0.5"; input.companion->trades.append(future);
    }
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.comparison.state.available);
    QCOMPARE(result.comparison.state.fromMs,Start+Minute); QCOMPARE(result.comparison.state.throughMs,Start+2*Minute);
    QCOMPARE(result.comparison.spotSamples,30); QCOMPARE(result.comparison.futuresSamples,30);
    QVERIFY(result.comparison.spotSignedFraction.has_value()); QVERIFY(result.comparison.futuresSignedFraction.has_value());
    QVERIFY(std::abs(*result.comparison.spotSignedFraction-1.0/3.0)<1e-12);
    QVERIFY(std::abs(*result.comparison.futuresSignedFraction+1.0/3.0)<1e-12);
    QCOMPARE(result.comparison.spotSignedBaseQuantity,QString("10"));
    QCOMPARE(result.comparison.futuresSignedBaseQuantity,QString("-10"));
    QVERIFY(!result.comparison.directionsAgree);
    QCOMPARE(result.comparison.spotGrain,CoinFlowGrain::SpotAggregate);
    QCOMPARE(result.comparison.futuresGrain,CoinFlowGrain::Futures100msAggregate);
    QVERIFY(!result.largeTrade.state.available); // No synthetic combined 60-sample pool.
    input.companion->instrument.baseAsset="OTHER";
    const auto mismatch=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(!mismatch.comparison.state.available); QVERIFY(mismatch.taker.state.available);
}
void CoinFlowStudiesTest::partialLiquidationCoverageIsAnObservedListRatherThanMarketTotal() {
    auto input=snapshot(); input.companion=CoinFlowWindow{};
    input.companion->instrument=instrument(CoinFlowVenue::BinanceUsdM);
    CoinLiquidationObservation item;
    item.symbol="SYNUSDT"; item.side="BUY"; item.price="100"; item.averagePrice="99";
    item.originalQuantity="5"; item.lastFilledQuantity="1"; item.cumulativeFilledQuantity="2";
    item.orderStatus="PARTIALLY_FILLED"; item.tradeAtMs=Start+1000; item.eventAtMs=Start+1001;
    item.receivedAtMs=item.knownAtMs=Start+1002; item.epoch=1; item.evidence=evidence(item.knownAtMs);
    input.liquidations={item};
    input.liquidationCoverage={{Start,Start+Minute,Start+Minute+1,1,CoinFlowCoverage::Partial,"합성 재연결 누락"}};
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.liquidation.state.available); QCOMPARE(result.liquidation.state.coverage,CoinFlowCoverage::Partial);
    QVERIFY(!result.liquidation.state.reason.isEmpty()); QCOMPARE(result.liquidation.state.samples,1);
    QCOMPARE(result.liquidation.observations.size(),1); QVERIFY(result.liquidation.sampledLastPerSecond);
    QCOMPARE(result.liquidation.observations.first().side,QString("BUY"));
}
void CoinFlowStudiesTest::latestPartialCompanionCannotFallBackToAnOlderCompleteComparison() {
    auto input=snapshot(3);
    observed(input.primary,Start+Minute,Start+2*Minute);
    observed(input.primary,Start+2*Minute,Start+3*Minute);
    input.companion=CoinFlowWindow{}; input.companion->instrument=instrument(CoinFlowVenue::BinanceUsdM);
    observed(*input.companion,Start+Minute,Start+2*Minute,7);
    input.companion->periods.append({Start+2*Minute,Start+3*Minute,Start+3*Minute+1,7,CoinFlowCoverage::Partial,"합성 최신 구간 누락"});
    for(int i=0;i<30;++i) {
        input.primary.trades.append(trade(i+1,Start+Minute+100+i*100,"1"));
        auto future=trade(i+1,Start+Minute+200+i*100,"1"); future.epoch=7;
        input.companion->trades.append(future);
    }
    const auto earlier=coinFlowStudies(input,Start+2*Minute+1000);
    QVERIFY(earlier.comparison.state.available);
    const auto latest=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(!latest.comparison.state.available);
    QVERIFY(!latest.comparison.state.reason.isEmpty());
}
void CoinFlowStudiesTest::independentCanonicalCandlesSurviveMissingFlowMetadata() {
    auto input=snapshot(21); input.minutes.last().candle.volume="40"; input.candleSeries=candleProof(input);
    input.primary={};
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.volume.state.available); QVERIFY(result.volume.multiple.has_value()); QCOMPARE(*result.volume.multiple,4.0);
    QVERIFY(hasKind(result,CoinFlowSignalKind::VolumeBurst));
    QVERIFY(!result.taker.state.available); QVERIFY(!result.largeTrade.state.available); QVERIFY(!result.openInterest.state.available);
    auto reversal=snapshot(4);
    const QString closes[]={"100","96","94","95"};
    for(int i=0;i<4;++i) reversal.minutes[i].candle={Start+i*Minute,0,closes[i],closes[i],closes[i],closes[i],"10"};
    reversal.minutes.last().candle.open=reversal.minutes.last().candle.low="94";
    reversal.candleSeries=candleProof(reversal); reversal.primary={};
    const auto bounce=coinFlowStudies(reversal,reversal.minutes.last().decisionAtMs);
    QVERIFY(bounce.reversal.state.available); QVERIFY(bounce.reversal.rebound); QVERIFY(hasKind(bounce,CoinFlowSignalKind::Rebound));
}
void CoinFlowStudiesTest::forgedSpotMetadataCannotRelabelOrEnableFlow() {
    auto input=snapshot(21); input.minutes.last().candle.volume="40"; input.candleSeries=candleProof(input);
    observed(input.primary,Start+20*Minute,Start+21*Minute);
    for(int i=0;i<30;++i) input.primary.trades.append(trade(i+1,Start+20*Minute+100+i*100,"1"));
    const auto baseline=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(baseline.taker.state.available); QVERIFY(baseline.volume.state.available);
    input.primary.instrument.baseAsset="OTHER";
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(!result.taker.state.available); QVERIFY(!result.cvd.state.available);
    QVERIFY(!hasKind(result,CoinFlowSignalKind::TakerSkew));
    QVERIFY(result.volume.state.available); QVERIFY(hasKind(result,CoinFlowSignalKind::VolumeBurst));
}
void CoinFlowStudiesTest::invalidCatalogProvenanceHidesFlowOnly_data() {
    QTest::addColumn<QString>("fault");
    for(const auto* name:{"arbitrary-url","wrong-host","userinfo","fragment","future-receipt","zero-receipt","known-before-receipt","future-source"})
        QTest::newRow(name)<<QString(name);
}
void CoinFlowStudiesTest::invalidCatalogProvenanceHidesFlowOnly() {
    QFETCH(QString,fault);
    auto input=snapshot(21); input.minutes.last().candle.volume="40"; input.candleSeries=candleProof(input);
    observed(input.primary,Start+20*Minute,Start+21*Minute);
    for(int i=0;i<30;++i) input.primary.trades.append(trade(i+1,Start+20*Minute+100+i*100,"1"));
    const qint64 now=input.minutes.last().decisionAtMs;
    const auto baseline=coinFlowStudies(input,now); QVERIFY(baseline.taker.state.available);
    auto& metadata=input.primary.instrument.evidence;
    if(fault=="arbitrary-url") metadata.sourceUrl="https://example.invalid/fake-catalog";
    if(fault=="wrong-host") metadata.sourceUrl="https://api.upbit.com/api/v3/exchangeInfo?permissions=SPOT&showPermissionSets=false";
    if(fault=="userinfo") metadata.sourceUrl="https://FAKE_PUBLIC_CANARY@api.binance.com/api/v3/exchangeInfo?permissions=SPOT&showPermissionSets=false";
    if(fault=="fragment") metadata.sourceUrl+="#FAKE_PUBLIC_CANARY";
    if(fault=="future-receipt") metadata.receivedAtMs=metadata.knownAtMs=now+1;
    if(fault=="zero-receipt") metadata.receivedAtMs=0;
    if(fault=="known-before-receipt") metadata.knownAtMs=metadata.receivedAtMs-1;
    if(fault=="future-source") metadata.sourceTimeMs=metadata.knownAtMs+1;
    const auto result=coinFlowStudies(input,now);
    QVERIFY(!result.taker.state.available); QVERIFY(!result.cvd.state.available);
    QVERIFY(!hasKind(result,CoinFlowSignalKind::TakerSkew));
    QVERIFY(result.volume.state.available); QVERIFY(hasKind(result,CoinFlowSignalKind::VolumeBurst));
}
void CoinFlowStudiesTest::verifiedAstralBaseAtOfficialScalarBoundaryKeepsObservedFlow() {
    auto input=snapshot();
    const auto base=QString::fromUcs4(U"\U00010400").repeated(36);
    const auto symbol=base+"USDT";
    QCOMPARE(base.size(),qsizetype(72)); QCOMPARE(symbol.toUcs4().size(),qsizetype(40));
    QVERIFY(binanceIdentifierValid(base)); QVERIFY(binanceIdentifierValid(symbol,2));
    input.primary.instrument.baseAsset=base; input.primary.instrument.symbol=symbol;
    input.primary.instrument.identity="coin:binance:"+symbol; input.identity=input.primary.instrument.identity;
    observed(input.primary,Start,Start+Minute);
    for(int i=0;i<30;++i) input.primary.trades.append(trade(i+1,Start+100+i*100,"1",i<20?CoinFlowSide::Buy:CoinFlowSide::Sell));
    const auto result=coinFlowStudies(input,input.minutes.last().decisionAtMs);
    QVERIFY(result.cvd.state.available); QVERIFY(!result.cvd.points.isEmpty());
    QCOMPARE(result.cvd.points.last().signedBaseQuantity,QString("10"));
    QVERIFY(result.taker.state.available); QCOMPARE(result.taker.state.samples,30);
    QVERIFY(result.taker.signedFraction.has_value()); QVERIFY(std::abs(*result.taker.signedFraction-1.0/3.0)<1e-12);
}

// The collector now trims its oldest whole minutes (trades and their coverage) instead of wiping
// a full window. Trimming a prefix may only remove the oldest results; it never changes a result
// of a later minute.
void CoinFlowStudiesTest::prefixTrimCannotChangeLaterSignalEntries() {
    auto full=snapshot(30);
    int id=1;
    for(int m=0;m<30;++m){observed(full.primary,Start+m*Minute,Start+(m+1)*Minute);
        for(int i=0;i<250;++i){const auto at=Start+m*Minute+100+i*200;
            full.primary.trades.append(trade(id,at,id%97==0?"1000":QString::number(5+id%7),id%3==0?CoinFlowSide::Sell:CoinFlowSide::Buy));++id;}}
    const auto decision=full.minutes.last().decisionAtMs;
    const auto before=coinFlowStudies(full,decision);
    QVERIFY(hasKind(before,CoinFlowSignalKind::LargeTrade));
    const qint64 boundary=Start+8*Minute;
    auto trimmed=full;
    trimmed.primary.trades.erase(std::remove_if(trimmed.primary.trades.begin(),trimmed.primary.trades.end(),[&](const auto& row){return row.tradeAtMs<boundary;}),trimmed.primary.trades.end());
    trimmed.primary.periods.erase(std::remove_if(trimmed.primary.periods.begin(),trimmed.primary.periods.end(),[&](const auto& period){return period.throughMs<=boundary;}),trimmed.primary.periods.end());
    const auto after=coinFlowStudies(trimmed,decision);
    const auto same=[](const CoinFlowSignal& a,const CoinFlowSignal& b){
        return a.kind==b.kind&&a.direction==b.direction&&a.atMs==b.atMs&&a.knownAtMs==b.knownAtMs&&a.entryClose==b.entryClose&&a.value==b.value;};
    for(const auto& entry:after.entries){
        QVERIFY2(entry.atMs>=boundary,"No result is produced for a trimmed minute");
        QVERIFY2(std::any_of(before.entries.cbegin(),before.entries.cend(),[&](const auto& other){return same(entry,other);}),"A kept minute's result is unchanged");
    }
    // 250 trades a minute: from the second kept minute on, every 200-trade baseline is fully held.
    int later=0;
    for(const auto& entry:before.entries)if(entry.atMs>=boundary+2*Minute){++later;
        QVERIFY2(std::any_of(after.entries.cbegin(),after.entries.cend(),[&](const auto& other){return same(entry,other);}),"A later result survives the trim");}
    QVERIFY(later>0);QVERIFY(hasKind(after,CoinFlowSignalKind::LargeTrade));
    QCOMPARE(after.largeTrade.latestPercentile,before.largeTrade.latestPercentile);
}
QTEST_GUILESS_MAIN(CoinFlowStudiesTest)
#include "tst_coin_flow_studies.moc"
