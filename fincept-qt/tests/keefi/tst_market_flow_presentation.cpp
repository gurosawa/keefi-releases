#include "keefi/MarketFlowPresentation.h"
#include <QTest>
using namespace keefi;
namespace {
constexpr qint64 Now=1792992120000; // 2026-10-26 05:22 UTC.
constexpr qint64 PreviousStart=1792900800000, PreviousClose=1792904400000;
MarketCandleSeries historical(QString asset,QString currency,QString identity,QString price,QString url) {
    MarketCandleSeries out;out.available=true;out.asset=asset;out.currency=currency;out.identity=identity;
    out.sourceUrl=url;out.receivedAtMs=Now-500;out.intervalMs=3600000;
    const auto tick=identity.startsWith("coin:upbit:")?PreviousClose-1000:0;
    out.sourceTimeMs=tick;out.candles.append({PreviousStart,tick,price,price,price,price,"1"});return out;
}
MarketCandleSeries historicalLocal() {return historical("BTC","KRW","coin:upbit:KRW-BTC","142800000","https://api.upbit.com/v1/candles/minutes/60?market=KRW-BTC&count=200");}
MarketCandleSeries historicalRemote() {return historical("BTC","USDT","coin:binance:BTCUSDT","100000","https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1h&limit=200");}
MarketCandleSeries historicalBridge() {return historical("USDT","KRW","coin:upbit:KRW-USDT","1400","https://api.upbit.com/v1/candles/minutes/60?market=KRW-USDT&count=200");}
ExchangePriceGap currentGap(const QString& localPrice="144200000") {
    const auto quote=[](QString base,QString currency,QString price){return AssetQuote{base,currency,price,{"Official public","https://example.test/public","public",Now-1000,Now-500,true}};};
    return MarketMetrics::exchangePriceGap(quote("BTC","KRW",localPrice),quote("BTC","USDT","100000"),quote("USDT","KRW","1400"),Now);
}
PublicDerivativesMetrics funding(const QString& rate="0.0001",int hours=8) {
    PublicDerivativesMetrics value;value.symbol="BTCUSDT";value.baseAsset="BTC";value.fundingRate=rate;value.fundingIntervalHours=hours;value.nextFundingTimeMs=Now+3600000;
    value.fundingEvidence={"Binance official","https://fapi.binance.com/fapi/v1/premiumIndex?symbol=BTCUSDT","public:binance:usdm:BTCUSDT",Now-1000,Now-500,true};
    value.intervalEvidence={"Binance official","https://fapi.binance.com/fapi/v1/fundingInfo","public:binance:usdm:BTCUSDT",0,Now-500,true};
    return value;
}
}
class MarketFlowPresentationTest final : public QObject {
    Q_OBJECT
  private slots:
    void dayChangeUsesCurrentFreshSameAssetAndExactOldWindow() {
        const auto previous=marketFlowHistoricalGap(historicalLocal(),historicalRemote(),historicalBridge(),Now);
        const auto current=currentGap();QVERIFY(current.available);QCOMPARE(current.percent,QString("3"));
        const auto comparison=marketFlowDayComparison(current,previous,Now);
        QVERIFY(comparison.available);QCOMPARE(comparison.previousPercent,QString("2"));
        QCOMPARE(comparison.differencePercentagePoints,QString("1"));QCOMPARE(comparison.comparisonCloseMs,PreviousClose);
        auto old=previous;old.percent="-1";QCOMPARE(marketFlowDayComparison(current,old,Now).differencePercentagePoints,QString("4"));
        old=previous;QCOMPARE(marketFlowDayComparison(currentGap("138600000"),old,Now).differencePercentagePoints,QString("-3"));
        old.percent="-3";QCOMPARE(marketFlowDayComparison(currentGap("138600000"),old,Now).differencePercentagePoints,QString("2"));
        old.percent="-1";QCOMPARE(marketFlowDayComparison(currentGap("138600000"),old,Now).differencePercentagePoints,QString("0"));
        old=previous;old.available=false;QVERIFY(!marketFlowDayComparison(current,old,Now).available);
        old=previous;old.asset="ETH";QVERIFY(!marketFlowDayComparison(current,old,Now).available);
        old=previous;old.percent="NaN";QVERIFY(!marketFlowDayComparison(current,old,Now).available);
        old=previous;old.comparisonCloseMs-=3600000;QVERIFY(!marketFlowDayComparison(current,old,Now).available);
        old=previous;old.receivedAtMs=0;QVERIFY(!marketFlowDayComparison(current,old,Now).available);
        old=previous;old.receivedAtMs=Now+1;QVERIFY(!marketFlowDayComparison(current,old,Now).available);
        old=previous;old.receivedAtMs=Now-3600001;QVERIFY(!marketFlowDayComparison(current,old,Now).available);
        auto live=current;live.available=false;QVERIFY(!marketFlowDayComparison(live,previous,Now).available);
        live=current;live.percent="4";QVERIFY(!marketFlowDayComparison(live,previous,Now).available);
        live=current;live.legs.removeLast();QVERIFY(!marketFlowDayComparison(live,previous,Now).available);
        live=current;live.legs.first().evidence.sourceTimeMs=Now-180001;QVERIFY(!marketFlowDayComparison(live,previous,Now).available);
        live=current;live.legs.first().quoteAsset="USD";QVERIFY(!marketFlowDayComparison(live,previous,Now).available);
        live=current;live.asOfMs=Now+1;QVERIFY(!marketFlowDayComparison(live,previous,Now).available);
        QVERIFY(!marketFlowDayComparison(current,previous,Now+3600000).available); // A new target hour cannot reuse yesterday's earlier close.
        QVERIFY(!marketFlowDayComparison(current,previous,0).available);
    }
    void dayChangeShowsFinalTruncatedSignedPercentagePoints_data() {
        QTest::addColumn<QString>("localPrice");QTest::addColumn<QString>("oldPercent");QTest::addColumn<QString>("difference");
        QTest::newRow("fraction-positive")<<QString("144200000")<<QString("2.123456")<<QString("0.876544");
        QTest::newRow("fraction-cross-zero")<<QString("138600000")<<QString("0.123456")<<QString("-1.123456");
        QTest::newRow("both-negative")<<QString("138600000")<<QString("-1.123456")<<QString("0.123456");
        QTest::newRow("negative-zero")<<QString("140000000")<<QString("-0.0000")<<QString("0");
    }
    void dayChangeShowsFinalTruncatedSignedPercentagePoints() {
        QFETCH(QString,localPrice);QFETCH(QString,oldPercent);QFETCH(QString,difference);
        auto old=marketFlowHistoricalGap(historicalLocal(),historicalRemote(),historicalBridge(),Now);old.percent=oldPercent;
        const auto value=marketFlowDayComparison(currentGap(localPrice),old,Now);QVERIFY(value.available);
        QCOMPARE(value.differencePercentagePoints,difference);
    }
    void historyRejectsMissingMisalignedOrUntrustedFacts_data() {
        QTest::addColumn<int>("leg");QTest::addColumn<QString>("mutation");QTest::addColumn<bool>("available");
        for(int leg=0;leg<3;++leg){
            for(const auto mutation:{"unavailable","asset","currency","identity","interval","url","missing","forming-only","duplicate","misaligned","wrong-hour","zero-price","receipt-zero","receipt-future","receipt-stale","source-future","tick-outside"})
                QTest::newRow(qPrintable(QString::number(leg)+":"+mutation))<<leg<<QString(mutation)<<false;
            QTest::newRow(qPrintable(QString::number(leg)+":receipt-boundary"))<<leg<<QString("receipt-boundary")<<true;
            QTest::newRow(qPrintable(QString::number(leg)+":forming-ignored"))<<leg<<QString("forming-ignored")<<true;
        }
    }
    void historyRejectsMissingMisalignedOrUntrustedFacts() {
        QFETCH(int,leg);QFETCH(QString,mutation);QFETCH(bool,available);
        auto a=historicalLocal(),b=historicalRemote(),c=historicalBridge();auto& value=leg==0?a:leg==1?b:c;
        if(mutation=="unavailable")value.available=false;
        if(mutation=="asset")value.asset="ETH";
        if(mutation=="currency")value.currency="USD";
        if(mutation=="identity")value.identity="coin:upbit:KRW-ETH";
        if(mutation=="interval")value.intervalMs=900000;
        if(mutation=="url")value.sourceUrl="https://example.test/private";
        if(mutation=="missing")value.candles.clear();
        if(mutation=="forming-only"){value.forming=value.candles.first();value.candles.clear();}
        if(mutation=="duplicate")value.candles.append(value.candles.first());
        if(mutation=="misaligned")++value.candles.first().startsAtMs;
        if(mutation=="wrong-hour")value.candles.first().startsAtMs-=3600000;
        if(mutation=="zero-price")value.candles.first().close="0";
        if(mutation=="receipt-zero")value.receivedAtMs=0;
        if(mutation=="receipt-future")value.receivedAtMs=Now+1;
        if(mutation=="receipt-stale")value.receivedAtMs=Now-3600001;
        if(mutation=="receipt-boundary"){value.receivedAtMs=Now-3600000;value.sourceTimeMs=qMin(value.sourceTimeMs,value.receivedAtMs);}
        if(mutation=="source-future")value.sourceTimeMs=Now+1;
        if(mutation=="tick-outside")value.candles.first().lastStoredTickMs=PreviousClose;
        if(mutation=="forming-ignored"){auto row=value.candles.first();row.startsAtMs=Now/3600000*3600000;row.close="999999999";value.forming=row;}
        QCOMPARE(marketFlowHistoricalGap(a,b,c,Now).available,available);
        QVERIFY(!marketFlowHistoricalGap(a,b,c,0).available);
    }
    void historyUsesThreeActuallyClosedMatchingHourRows() {
        const auto local=historicalLocal(),remote=historicalRemote(),bridge=historicalBridge();
        const auto value=marketFlowHistoricalGap(local,remote,bridge,Now);
        QVERIFY(value.available);QCOMPARE(value.percent,QString("2"));
        QCOMPARE(value.comparisonCloseMs,PreviousClose);QCOMPARE(value.receivedAtMs,Now-500);QCOMPARE(value.asset,QString("BTC"));
        // Original source clocks remain historical; no synthetic quote/evidence is needed.
        QCOMPARE(local.candles.first().lastStoredTickMs,PreviousClose-1000);QCOMPARE(remote.candles.first().lastStoredTickMs,qint64(0));
        QCOMPARE(local.receivedAtMs,Now-500);QCOMPARE(local.sourceTimeMs,PreviousClose-1000);
    }
    void openInterestUsesDirectionPairWithoutHeatWords() {
        for(const auto value:{"-100", "0", "100"})
            QCOMPARE(marketFlowState(MarketFlowMetric::OpenInterest,value),MarketFlowState::Unavailable);
        QCOMPARE(marketFlowMetricLabel(MarketFlowMetric::OpenInterest),QStringLiteral("미결제약정 추이"));
        ContractFlow value;value.available=true;value.priceDirection=1;value.quantityDirection=-1;
        QCOMPARE(marketFlowContractLabel(value),QStringLiteral("가격 상승 · 미결제약정 하락"));
        value.available=false;QVERIFY(marketFlowContractLabel(value).isEmpty());
        value.available=true;value.priceDirection=2;QVERIFY(marketFlowContractLabel(value).isEmpty());
        value.priceDirection=0;value.quantityDirection=-2;QVERIFY(marketFlowContractLabel(value).isEmpty());
        const QStringList words{QStringLiteral("하락"),QStringLiteral("보합"),QStringLiteral("상승")};
        for(int price=-1;price<=1;++price)for(int quantity=-1;quantity<=1;++quantity){
            value.priceDirection=price;value.quantityDirection=quantity;
            QCOMPARE(marketFlowContractLabel(value),QStringLiteral("가격 %1 · 미결제약정 %2").arg(words[price+1],words[quantity+1]));
        }
    }
    void fundingStateNeedsFreshKnownPublicPeriod() {
        QCOMPARE(marketFlowFundingState(funding("0.000274"),Now),MarketFlowState::Hot); // 30.003%.
        QCOMPARE(marketFlowFundingState(funding("0.000273"),Now),MarketFlowState::Normal); // 29.8935%.
        QCOMPARE(marketFlowFundingState(funding("0"),Now),MarketFlowState::Normal);
        const auto tiny=funding("-0.000000000001");QCOMPARE(marketFlowAnnualFundingPercent(tiny.fundingRate,8),QString("0"));
        QCOMPARE(marketFlowFundingState(tiny,Now),MarketFlowState::Cool); // Classification cannot round a negative ratio to zero.
        QCOMPARE(marketFlowFundingState(funding("0.0001",0),Now),MarketFlowState::Unavailable);
        QCOMPARE(marketFlowFundingState(funding("0.0001",25),Now),MarketFlowState::Unavailable);
        auto value=funding();value.fundingEvidence.sourceTimeMs=Now-MarketMetrics::FreshnessMs;
        QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Normal);
        --value.fundingEvidence.sourceTimeMs;QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
        value=funding();value.intervalEvidence.receivedAtMs=Now-MarketMetrics::FreshnessMs;
        QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Normal);
        --value.intervalEvidence.receivedAtMs;QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
        value=funding();value.fundingEvidence.sourceTimeMs=Now+1;QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
        value=funding();value.fundingEvidence.receivedAtMs=Now+1;QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
        value=funding();value.intervalEvidence.complete=false;QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
        value=funding();value.fundingEvidence.complete=false;QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
        value=funding();value.fundingEvidence.scope="private:account";QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
        value=funding();value.intervalEvidence.scope="private:account";QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
        value=funding();value.nextFundingTimeMs=Now;QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
        value=funding();value.fundingRate.clear();QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
        value=funding();value.baseAsset.clear();QCOMPARE(marketFlowFundingState(value,Now),MarketFlowState::Unavailable);
    }
    void publishedFundingAnnualization_data() {
        QTest::addColumn<QString>("rate");QTest::addColumn<int>("hours");QTest::addColumn<QString>("annual");
        QTest::newRow("8h")<<QString("0.0001")<<8<<QString("10.95");
        QTest::newRow("4h")<<QString("0.0001")<<4<<QString("21.9");
        QTest::newRow("1h")<<QString("0.0001")<<1<<QString("87.6");
        QTest::newRow("3h")<<QString("0.0001")<<3<<QString("29.2");
        QTest::newRow("7h-truncated")<<QString("0.0001")<<7<<QString("12.514285");
        QTest::newRow("negative")<<QString("-0.000125")<<4<<QString("-27.375");
        QTest::newRow("zero")<<QString("0")<<8<<QString("0");
        QTest::newRow("tiny-negative-display-zero")<<QString("-0.000000000001")<<8<<QString("0");
        QTest::newRow("unknown-period")<<QString("0.0001")<<0<<QString{};
        QTest::newRow("invalid-period")<<QString("0.0001")<<25<<QString{};
        QTest::newRow("missing")<<QString{}<<8<<QString{};
        QTest::newRow("non-decimal")<<QString("1e-4")<<8<<QString{};
    }
    void publishedFundingAnnualization() {
        QFETCH(QString,rate);QFETCH(int,hours);QFETCH(QString,annual);
        QCOMPARE(marketFlowAnnualFundingPercent(rate,hours),annual);
    }
    void exactStateBoundaries_data() {
        QTest::addColumn<MarketFlowMetric>("metric");QTest::addColumn<QString>("percent");QTest::addColumn<MarketFlowState>("state");
        const auto row=[](const char* name,MarketFlowMetric metric,const char* value,MarketFlowState state){QTest::newRow(name)<<metric<<QString::fromLatin1(value)<<state;};
        row("kr-below-cool",MarketFlowMetric::KoreaGap,"-0.500001",MarketFlowState::Cool);
        row("kr-at-cool",MarketFlowMetric::KoreaGap,"-0.5",MarketFlowState::Cool);
        row("kr-above-cool",MarketFlowMetric::KoreaGap,"-0.499999",MarketFlowState::Normal);
        row("kr-below-hot",MarketFlowMetric::KoreaGap,"2.999999",MarketFlowState::Normal);
        row("kr-at-hot",MarketFlowMetric::KoreaGap,"3",MarketFlowState::Hot);
        row("kr-above-hot",MarketFlowMetric::KoreaGap,"3.000001",MarketFlowState::Hot);
        row("funding-negative",MarketFlowMetric::Funding,"-0.000000000001",MarketFlowState::Cool);
        row("funding-zero",MarketFlowMetric::Funding,"0",MarketFlowState::Normal);
        row("funding-below-hot",MarketFlowMetric::Funding,"29.999999",MarketFlowState::Normal);
        row("funding-at-hot",MarketFlowMetric::Funding,"30",MarketFlowState::Hot);
        row("funding-above-hot",MarketFlowMetric::Funding,"30.000001",MarketFlowState::Hot);
        row("cb-below-cool",MarketFlowMetric::CoinbaseGap,"-0.100001",MarketFlowState::Cool);
        row("cb-at-cool",MarketFlowMetric::CoinbaseGap,"-0.1",MarketFlowState::Cool);
        row("cb-above-cool",MarketFlowMetric::CoinbaseGap,"-0.099999",MarketFlowState::Normal);
        row("cb-below-hot",MarketFlowMetric::CoinbaseGap,"0.099999",MarketFlowState::Normal);
        row("cb-at-hot",MarketFlowMetric::CoinbaseGap,"0.1",MarketFlowState::Hot);
        row("cb-above-hot",MarketFlowMetric::CoinbaseGap,"0.100001",MarketFlowState::Hot);
        for(const auto metric:{MarketFlowMetric::KoreaGap,MarketFlowMetric::Funding,MarketFlowMetric::CoinbaseGap}){
            const auto id=QString::number(int(metric));
            for(const auto value:{"","NaN","inf","1e2","+3"," 3","3%","--1","3\n"})QTest::newRow(qPrintable(id+":"+value))<<metric<<QString::fromLatin1(value)<<MarketFlowState::Unavailable;
            QTest::newRow(qPrintable(id+":negative-zero"))<<metric<<QString("-0.0000")<<MarketFlowState::Normal;
        }
        QTest::newRow("invalid-metric")<<MarketFlowMetric(999)<<QString("3")<<MarketFlowState::Unavailable;
    }
    void exactStateBoundaries() {
        QFETCH(MarketFlowMetric,metric);QFETCH(QString,percent);QFETCH(MarketFlowState,state);
        QCOMPARE(marketFlowState(metric,percent),state);
    }
    void humanStateLabelsUseCentralTemporaryCopy() {
        QCOMPARE(marketFlowMetricLabel(MarketFlowMetric::KoreaGap),QStringLiteral("테더 환산 가격차"));
        QCOMPARE(marketFlowMetricLabel(MarketFlowMetric::Funding),QStringLiteral("펀딩비"));
        QCOMPARE(marketFlowMetricLabel(MarketFlowMetric::CoinbaseGap),QStringLiteral("코인베이스 프리미엄"));
        QCOMPARE(marketFlowStateLabel(MarketFlowState::Cool),QStringLiteral("침체"));
        QCOMPARE(marketFlowStateLabel(MarketFlowState::Normal),QStringLiteral("보통"));
        QCOMPARE(marketFlowStateLabel(MarketFlowState::Hot),QStringLiteral("과열"));
    }
    void stateAtKoreaGapHotBoundary() {
        QCOMPARE(marketFlowState(MarketFlowMetric::KoreaGap, "3.0"), MarketFlowState::Hot);
    }
    void hourlySeriesUsesOnlyCompletedHoursWhereAllThreeClosesValidate() {
        const auto lastStart=Now/3600000*3600000-3600000;
        const auto hours=[&](QString asset,QString currency,QString identity,QString price,QString url,qint64 skip){
            auto out=historical(asset,currency,identity,price,url);out.candles.clear();const bool upbit=identity.startsWith("coin:upbit:");
            for(auto start=PreviousStart;start<=lastStart;start+=3600000){if(start==skip)continue;const auto tick=upbit?start+3600000-1000:0;out.candles.append({start,tick,price,price,price,price,"1"});out.sourceTimeMs=qMax(out.sourceTimeMs,tick);}
            // The forming hour is never a completed point.
            out.forming=MarketCandle{lastStart+3600000,upbit?Now-1000:0,"1","1","1","1","1"};return out;};
        const auto missing=PreviousStart+10*3600000;
        const auto local=hours("BTC","KRW","coin:upbit:KRW-BTC","142800000","https://api.upbit.com/v1/candles/minutes/60?market=KRW-BTC&count=200",0);
        const auto remote=hours("BTC","USDT","coin:binance:BTCUSDT","100000","https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1h&limit=200",0);
        const auto bridge=hours("USDT","KRW","coin:upbit:KRW-USDT","1400","https://api.upbit.com/v1/candles/minutes/60?market=KRW-USDT&count=200",missing);
        const auto value=marketFlowHistoricalGap(local,remote,bridge,Now);
        QVERIFY(value.available);QCOMPARE(value.percent,QString("2"));QCOMPARE(value.series.size(),24);
        QCOMPARE(value.series.first().closeMs,PreviousClose);QCOMPARE(value.series.last().closeMs,lastStart+3600000);
        for(const auto& point:value.series){QVERIFY(point.closeMs!=missing+3600000);QCOMPARE(point.percent,QString("2"));}
        // A window that had not closed when the rows were received is not a point.
        auto late=bridge;auto lateLocal=local,lateRemote=remote;late.receivedAtMs=lateLocal.receivedAtMs=lateRemote.receivedAtMs=lastStart+3600000-1;
        const auto partial=marketFlowHistoricalGap(lateLocal,lateRemote,late,Now);QVERIFY(partial.available);QCOMPARE(partial.series.last().closeMs,lastStart);
    }
    void displayRoundingIsHalfAwayFromZeroAndKeepsPolicyExact_data() {
        QTest::addColumn<QString>("exact");QTest::addColumn<int>("decimals");QTest::addColumn<QString>("shown");
        QTest::newRow("negative")<<"-0.07944"<<2<<"-0.08";QTest::newRow("integer")<<"2"<<2<<"2.00";QTest::newRow("tiny")<<"0.004"<<2<<"0.00";
        QTest::newRow("tiny-negative")<<"-0.004"<<2<<"0.00";QTest::newRow("half")<<"43.85"<<1<<"43.9";QTest::newRow("annual")<<"43.8"<<1<<"43.8";QTest::newRow("invalid")<<"bad"<<2<<"";
    }
    void displayRoundingIsHalfAwayFromZeroAndKeepsPolicyExact() {
        QFETCH(QString,exact);QFETCH(int,decimals);QFETCH(QString,shown);QCOMPARE(marketFlowRounded(exact,decimals),shown);
        // Rounding is display only: 2.996 shows 3.00 but stays below the hot boundary.
        QCOMPARE(marketFlowRounded("2.996",2),QString("3.00"));QCOMPARE(marketFlowState(MarketFlowMetric::KoreaGap,"2.996"),MarketFlowState::Normal);
    }
    void temporaryCopyHelpersChooseParticlesAndKoreanReleaseNames() {
        QCOMPARE(usabilityParticle(QStringLiteral("김프"),QStringLiteral("이"),QStringLiteral("가")),QStringLiteral("김프가"));
        QCOMPARE(usabilityParticle(QStringLiteral("펀딩"),QStringLiteral("이"),QStringLiteral("가")),QStringLiteral("펀딩이"));
        QCOMPARE(usabilityParticle(QStringLiteral("미국 소비자물가(CPI)"),QStringLiteral("이"),QStringLiteral("가")),QStringLiteral("미국 소비자물가(CPI)가"));
        QCOMPARE(usabilityParticle(QStringLiteral("GDP"),QStringLiteral("이"),QStringLiteral("가")),QStringLiteral("GDP이(가)"));
        QCOMPARE(usabilityCalendarTitle("GDP (Advance Estimate), 3rd Quarter 2026"),QStringLiteral("미국 GDP 속보치"));
        QCOMPARE(usabilityCalendarTitle("Personal Income and Outlays, September 2026"),QStringLiteral("미국 개인소득·소비(PCE 물가)"));
        QCOMPARE(usabilityCalendarTitle("Consumer Price Index for September 2026"),QStringLiteral("미국 소비자물가(CPI)"));
        QCOMPARE(usabilityCalendarTitle("Employment Cost Index for September 2026"),QStringLiteral("미국 고용비용지수"));
        QCOMPARE(usabilityCalendarTitle("Unlisted release"),QString("Unlisted release"));
    }
};
QTEST_GUILESS_MAIN(MarketFlowPresentationTest)
#include "tst_market_flow_presentation.moc"
