#include "keefi/PortfolioMoneyDisplay.h"
#include <QTest>
#include <algorithm>

using namespace keefi;
namespace {
constexpr qint64 Now = 1793000000000;
AssetQuote directRate(const QString& value = QStringLiteral("1300")) {
    return {"USD", "KRW", value, {"Synthetic direct FX", "https://synthetic.invalid/USD-KRW", "public:synthetic:USD-KRW", Now - 1000, Now - 500, true}};
}
QList<AssetQuote> crossRates(const QString& krw = QStringLiteral("1300"), const QString& usd = QStringLiteral("0.998")) {
    return {{"USDT", "KRW", krw, {"Synthetic Upbit", "https://api.upbit.com/v1/ticker", "public:upbit:KRW-USDT", Now - 1000, Now - 500, true}},
            {"USDT", "USD", usd, {"Synthetic Coinbase", "https://api.exchange.coinbase.com/products/USDT-USD/ticker", "public:coinbase:USDT-USD", Now - 21000, Now - 500, true}}};
}
}
class MoneyDisplayTest final : public QObject {
    Q_OBJECT
  private slots:
    void defaultKrwPreservesTheExistingFormatter() {
        PortfolioMoneyDisplay display;
        QCOMPARE(display.unit(), QString("KRW"));
        QCOMPARE(display.amount("1234567.5"), QString("1,234,568"));
        QCOMPARE(display.amount("-0.5"), QString("-1"));
        QVERIFY(!display.waitingForRate());
        QVERIFY(display.status().isEmpty());
        QCOMPARE(display.expiresAtMs(), qint64(0));
    }
    void freshDirectDollarRateChangesOnlyTheDisplay() {
        const QList<AssetQuote> quotes{directRate()};const auto original=quotes;
        const auto display=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(quotes,original);
        QCOMPARE(display.amount("130000"),QString("100.00"));
        QCOMPARE(display.unit(),QString("USD"));
        QVERIFY(!display.waitingForRate());
        QCOMPARE(display.expiresAtMs(),Now+179001);
        QVERIFY(display.status().isEmpty());
        QVERIFY(display.information().contains("달러 환율"));
        QVERIFY(display.information().contains("16:33")); // Independently calculated KST time of Now-1000.
        QVERIFY(!display.information().contains("https://"));
        QVERIFY(!display.information().contains("public:synthetic:"));
    }
    void existingPublicPairDisplaysDollarsRatherThanUsdt() {
        const auto quotes=crossRates();const auto original=quotes;
        const auto display=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(display.unit(),QString("USD"));QCOMPARE(display.amount("130000"),QString("99.80"));
        QVERIFY(!display.waitingForRate());QVERIFY(display.status().isEmpty());
        QCOMPARE(display.expiresAtMs(),Now+159001);
        QVERIFY(display.information().contains("코인베이스"));QVERIFY(display.information().contains("USDT/USD"));
        QVERIFY(display.information().contains("USDT/KRW"));QVERIFY(display.information().contains("16:32"));QVERIFY(display.information().contains("16:33"));
        QVERIFY(!display.information().contains("https://"));QVERIFY(!display.information().contains("public:"));QCOMPARE(quotes,original);
    }
    void crossConversionRoundsTheFinalExactRatio_data() {
        QTest::addColumn<QString>("krw");QTest::addColumn<QString>("won");QTest::addColumn<QString>("usd");QTest::addColumn<QString>("expected");
        const auto row=[](const char* name,const QString& krw,const QString& won,const QString& usd,const QString& expected){QTest::newRow(name)<<krw<<won<<usd<<expected;};
        // Literal oracles were checked independently with Python Decimal, precision 350 and ROUND_HALF_UP.
        row("half-exact-rational","1000.005","3","7","2,333.35");
        row("negative-half","-1000.005","3","7","-2,333.35");
        row("below-half","1000.004999","3","7","2,333.34");
        row("small-half","0.0025","1","2","0.01");
        row("tiny-coinbase","0.02","1","0.25","0.01");
        row("fractional-leg-rates","123456789.987654321","1300.12","0.9997","94,929.51");
        row("beyond-80-digit-intermediate",QString(80,'9'),"1300.12","0.9997","76,892,902,193,643,663,661,815,832,384,702,950,496,877,211,334,338,368,765,960,065,224,748,484,755,253.36");
        row("small-denominator","1","0."+QString(79,'0')+"1","0.998","99,800,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000.00");
        row("zero","0","1300","0.998","0.00");
        row("negative-zero","-0.000","1300","0.998","0.00");
    }
    void crossConversionRoundsTheFinalExactRatio() {
        QFETCH(QString,krw);QFETCH(QString,won);QFETCH(QString,usd);QFETCH(QString,expected);
        const auto quotes=crossRates(won,usd);const auto original=quotes;
        const auto display=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(display.unit(),QString("USD"));QCOMPARE(display.amount(krw),expected);QCOMPARE(quotes,original);
    }
    void unusableCrossLegsStayInKrw_data() {
        QTest::addColumn<int>("leg");QTest::addColumn<QString>("fault");
        for(int leg:{0,1})for(const auto* fault:{"missing","source-zero","receipt-zero","source-future","receipt-future","source-stale","receipt-stale",
            "incomplete","source-empty","url-empty","foreign-scope","private-scope","foreign-url","wrong-base","wrong-quote",
            "zero-price","negative-price","invalid-price","exponent-price","long-price","clock-lead-over-two-seconds"}){
            const auto name=QString(leg?"coinbase-":"upbit-")+QString::fromLatin1(fault);
            QTest::newRow(qPrintable(name))<<leg<<QString::fromLatin1(fault);
        }
    }
    void unusableCrossLegsStayInKrw() {
        QFETCH(int,leg);QFETCH(QString,fault);auto quotes=crossRates();auto& quote=quotes[leg];
        if(fault=="source-zero")quote.evidence.sourceTimeMs=0;
        if(fault=="receipt-zero")quote.evidence.receivedAtMs=0;
        if(fault=="source-future")quote.evidence.sourceTimeMs=Now+1;
        if(fault=="receipt-future")quote.evidence.receivedAtMs=Now+1;
        if(fault=="source-stale")quote.evidence.sourceTimeMs=Now-180001;
        if(fault=="receipt-stale"){quote.evidence.sourceTimeMs=Now-179000;quote.evidence.receivedAtMs=Now-180001;}
        if(fault=="incomplete")quote.evidence.complete=false;
        if(fault=="source-empty")quote.evidence.source=" ";
        if(fault=="url-empty")quote.evidence.url={};
        if(fault=="foreign-scope")quote.evidence.scope=leg?"public:other:USDT-USD":"public:other:KRW-USDT";
        if(fault=="private-scope")quote.evidence.scope="private:synthetic";
        if(fault=="foreign-url")quote.evidence.url+="?unapproved=1";
        if(fault=="wrong-base")quote.baseAsset="BTC";
        if(fault=="wrong-quote")quote.quoteAsset="EUR";
        if(fault=="zero-price")quote.price="0";
        if(fault=="negative-price")quote.price="-1";
        if(fault=="invalid-price")quote.price="NaN";
        if(fault=="exponent-price")quote.price="1e0";
        if(fault=="long-price")quote.price=QString(81,'1');
        if(fault=="clock-lead-over-two-seconds"){quote.evidence.sourceTimeMs=Now-1;quote.evidence.receivedAtMs=Now-2002;}
        if(fault=="missing")quotes.removeAt(leg);
        const auto original=quotes;const auto display=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(display.unit(),QString("KRW"));QVERIFY(display.waitingForRate());QCOMPARE(display.status(),QStringLiteral("환율 불러오는 중"));
        QCOMPARE(display.amount("130000"),QString("130,000"));QCOMPARE(display.expiresAtMs(),qint64(0));QCOMPARE(quotes,original);
    }
    void crossClocksKeepTheirInclusiveFreshnessAndThirtySecondSkew() {
        auto quotes=crossRates();quotes[0].evidence.sourceTimeMs=Now-180000;quotes[0].evidence.receivedAtMs=Now-179500;
        quotes[1].evidence.sourceTimeMs=Now-150000;quotes[1].evidence.receivedAtMs=Now-149500;
        const auto original=quotes;const auto boundary=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(boundary.unit(),QString("USD"));QCOMPARE(boundary.expiresAtMs(),Now+1);QCOMPARE(quotes,original);
        QVERIFY(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now+1).waitingForRate());
        quotes=crossRates();quotes[1].evidence.sourceTimeMs=Now-31000;
        QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now).unit(),QString("USD"));
        --quotes[1].evidence.sourceTimeMs;QVERIFY(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now).waitingForRate());
        quotes=crossRates();quotes[0].evidence.sourceTimeMs=Now-1;quotes[0].evidence.receivedAtMs=Now-2001;
        QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now).unit(),QString("USD"));
        quotes=crossRates();quotes[0].evidence.url="wss://api.upbit.com/websocket/v1";
        QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now).unit(),QString("USD"));
    }
    void latestCrossConflictsCannotPromoteAnOlderPair_data() {
        QTest::addColumn<int>("leg");QTest::newRow("upbit")<<0;QTest::newRow("coinbase")<<1;
    }
    void latestCrossConflictsCannotPromoteAnOlderPair() {
        QFETCH(int,leg);auto quotes=crossRates("2","1");auto conflicting=quotes[leg];conflicting.price=leg?"2":"3";quotes.append(conflicting);
        const auto original=quotes;QVERIFY(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now).waitingForRate());QCOMPARE(quotes,original);
        std::reverse(quotes.begin(),quotes.end());QVERIFY(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now).waitingForRate());
        auto newer=conflicting;newer.price=leg?"3":"4";newer.evidence.sourceTimeMs+=500;quotes.append(newer);
        const auto resolved=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(resolved.amount("130000"),leg?QString("195,000.00"):QString("32,500.00"));
        std::reverse(quotes.begin(),quotes.end());QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now),resolved);
    }
    void directEvidenceTakesPrecedenceAndItsConflictsCannotBeBypassed() {
        auto quotes=crossRates();quotes.append(directRate());
        const auto display=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(display.amount("130000"),QString("100.00"));QCOMPARE(display.expiresAtMs(),Now+179001);
        QVERIFY(display.information().contains("달러 환율"));QVERIFY(!display.information().contains("코인베이스"));
        auto conflict=directRate("1301");quotes.append(conflict);QVERIFY(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now).waitingForRate());
    }
    void crossPresentationKeepsTheOriginalQuoteClocksAndInput() {
        auto quotes=crossRates("1300","0.998");const auto original=quotes;
        const auto first=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now+1000),first);QCOMPARE(quotes,original);
        QVERIFY(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now+159001)!=first);
        auto normalized=quotes[1];normalized.price="00.99800";quotes.append(normalized);
        QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now).amount("130000"),QString("99.80"));
        QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Krw,quotes,Now),PortfolioMoneyDisplay{});
    }
    void centsUseExactSignedDecimalRounding_data() {
        QTest::addColumn<QString>("krw");QTest::addColumn<QString>("rate");QTest::addColumn<QString>("expected");
        const auto row=[](const char* name,const QString& krw,const QString& rate,const QString& expected){QTest::newRow(name)<<krw<<rate<<expected;};
        row("positive-half","1306.5","1300","1.01");
        row("negative-half","-1306.5","1300","-1.01");
        row("round-up-to-integer","1293.5","1300","1.00");
        row("just-below-half","1293.499","1300","0.99");
        row("zero","0","1300","0.00");
        row("negative-zero","-0.000","1300","0.00");
        row("small-positive-half","0.005","1","0.01");
        row("small-negative-half","-0.005","1","-0.01");
        row("below-positive-half","0.004999","1","0.00");
        row("below-negative-half","-0.004999","1","0.00");
        row("grouped-rounding","1234567.895","1","1,234,567.90");
        row("negative-grouped-rounding","-1234567.895","1","-1,234,567.90");
        row("decimal-rate","13000.000000000000000001","1.3","10,000.00");
        row("repeating-down","1","3","0.33");
        row("repeating-up","2","3","0.67");
        row("fraction-half","1","8","0.13");
        row("fraction-half-zero","1","40","0.03");
        row("carry-across-group","999.995","1","1,000.00");
        row("leading-trailing-zero","001300.0000","1300","1.00");
        row("large-80-digit-input",QString(80,'9'),"1","99,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999,999.00");
        row("output-beyond-80-digits","1","0."+QString(79,'0')+"1","100,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000.00");
        row("empty-is-not-zero",{},"1300",{});
        row("placeholder-is-not-zero",QString::fromUtf8("—"),"1300",QString::fromUtf8("—"));
        row("nan-is-not-zero","NaN","1300","NaN");
        row("exponent-is-not-parsed","1e3","1300","1e3");
        row("invalid-plus-is-preserved","+1","1300","+1");
    }
    void centsUseExactSignedDecimalRounding() {
        QFETCH(QString,krw);QFETCH(QString,rate);QFETCH(QString,expected);
        const QList<AssetQuote> quotes{directRate(rate)};const auto original=quotes;
        const auto display=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(display.unit(),QString("USD"));QCOMPARE(display.amount(krw),expected);QCOMPARE(quotes,original);
    }
    void unusableRatesStayInKrwWithoutInventingValues_data() {
        QTest::addColumn<QString>("fault");
        for(const auto* fault:{"no-rate","source-zero","receipt-zero","source-future","receipt-future","source-stale","receipt-stale",
            "incomplete","source-empty","url-empty","private-scope","empty-public-scope","usdt-is-not-usd","reverse-pair","wrong-quote",
            "zero-price","negative-price","invalid-price","exponent-price","long-price","clock-lead-over-two-seconds"})QTest::newRow(fault)<<QString::fromLatin1(fault);
    }
    void unusableRatesStayInKrwWithoutInventingValues() {
        QFETCH(QString,fault);auto quote=directRate();
        if(fault=="source-zero")quote.evidence.sourceTimeMs=0;
        if(fault=="receipt-zero")quote.evidence.receivedAtMs=0;
        if(fault=="source-future")quote.evidence.sourceTimeMs=Now+1;
        if(fault=="receipt-future")quote.evidence.receivedAtMs=Now+1;
        if(fault=="source-stale")quote.evidence.sourceTimeMs=Now-180001;
        if(fault=="receipt-stale"){quote.evidence.sourceTimeMs=Now-179000;quote.evidence.receivedAtMs=Now-180001;}
        if(fault=="incomplete")quote.evidence.complete=false;
        if(fault=="source-empty")quote.evidence.source=" ";
        if(fault=="url-empty")quote.evidence.url={};
        if(fault=="private-scope")quote.evidence.scope="private:synthetic";
        if(fault=="empty-public-scope")quote.evidence.scope="public:";
        if(fault=="usdt-is-not-usd")quote.baseAsset="USDT";
        if(fault=="reverse-pair"){quote.baseAsset="KRW";quote.quoteAsset="USD";}
        if(fault=="wrong-quote")quote.quoteAsset="USDT";
        if(fault=="zero-price")quote.price="0";
        if(fault=="negative-price")quote.price="-1300";
        if(fault=="invalid-price")quote.price="NaN";
        if(fault=="exponent-price")quote.price="1.3e3";
        if(fault=="long-price")quote.price=QString(81,'1');
        if(fault=="clock-lead-over-two-seconds"){quote.evidence.sourceTimeMs=Now-1;quote.evidence.receivedAtMs=Now-2002;}
        const QList<AssetQuote> quotes=fault=="no-rate"?QList<AssetQuote>{}:QList<AssetQuote>{quote};const auto original=quotes;
        const auto display=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(display.unit(),QString("KRW"));QCOMPARE(display.amount("-130000.5"),QString("-130,001"));
        QVERIFY(display.waitingForRate());QCOMPARE(display.status(),QString::fromUtf8("환율 불러오는 중"));QCOMPARE(display.expiresAtMs(),qint64(0));
        QVERIFY(display.information().contains("달러 환율"));QVERIFY(!display.information().contains("https://"));QCOMPARE(quotes,original);
    }
    void theOriginalClocksExpireAtTheExistingInclusiveBoundary() {
        auto quote=directRate();quote.evidence.sourceTimeMs=Now-180000;quote.evidence.receivedAtMs=Now-179500;
        const QList<AssetQuote> quotes{quote};const auto original=quotes;
        const auto valid=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(valid.unit(),QString("USD"));QCOMPARE(valid.expiresAtMs(),Now+1);
        const auto expired=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now+1);
        QCOMPARE(expired.unit(),QString("KRW"));QVERIFY(expired.waitingForRate());QCOMPARE(quotes,original);
        quote.evidence.sourceTimeMs=Now-500;quote.evidence.receivedAtMs=Now-2500;
        const auto leadBoundary=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,{quote},Now);
        QCOMPARE(leadBoundary.unit(),QString("USD"));QCOMPARE(leadBoundary.expiresAtMs(),Now+177501);
    }
    void latestDirectEvidenceRejectsConflictsAndKeepsNormalizedTies() {
        auto old=directRate("1400");old.evidence.sourceTimeMs=Now-2000;old.evidence.receivedAtMs=Now-1500;
        auto recent=directRate("1300");QList<AssetQuote> quotes{old,recent};const auto original=quotes;
        QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now).amount("130000"),QString("100.00"));
        QCOMPARE(quotes,original);
        auto normalized=recent;normalized.price="001300.000";quotes={recent,normalized};
        QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now).amount("130000"),QString("100.00"));
        normalized.price="1301";quotes={old,recent,normalized};const auto conflicting=quotes;
        const auto rejected=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QVERIFY(rejected.waitingForRate());QCOMPARE(rejected.unit(),QString("KRW"));QCOMPARE(rejected.amount("130000"),QString("130,000"));QCOMPARE(quotes,conflicting);
        auto newest=directRate("1500");newest.evidence.sourceTimeMs=Now-100;quotes.append(newest);
        const auto resolved=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(resolved.amount("150000"),QString("100.00"));
        std::reverse(quotes.begin(),quotes.end());QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now),resolved);
    }
    void cachedPresentationDoesNotChangeWithOnlyTheWallClock() {
        const QList<AssetQuote> quotes{directRate()};
        const auto first=PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now);
        QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now+1000),first);
        QVERIFY(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Usd,quotes,Now+179001)!=first);
        QCOMPARE(PortfolioMoneyDisplay::fromQuotes(MoneyCurrency::Krw,{},Now),PortfolioMoneyDisplay{});
    }
};
QTEST_APPLESS_MAIN(MoneyDisplayTest)
#include "tst_money_display.moc"
