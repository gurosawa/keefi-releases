#include "keefi/HoldingMarketResolver.h"
#include <QTest>
using namespace keefi;
namespace {
HoldingMarketCatalog catalog(){
    HoldingMarketCatalog value;PublicMarketRow row;row.market="KRW-SOL";row.base="SOL";
    value.upbitRows={row};value.upbitAvailable=true;
    MarketInstrument spot;spot.symbol="SOLUSDT";spot.baseAsset="SOL";spot.quoteAsset="USDT";
    spot.market="spot";spot.status="TRADING";spot.tradable=true;value.binance={spot};return value;
}
}
class HoldingMarketResolverTest final:public QObject {
    Q_OBJECT
private slots:
    void canonicalSolChoiceUsesPublicThenCustodyThenCurrency_data(){
        QTest::addColumn<QStringList>("custody");QTest::addColumn<QString>("currency");
        QTest::addColumn<QString>("publicFirst");QTest::addColumn<QString>("session");
        QTest::addColumn<bool>("upbitAvailable");QTest::addColumn<QString>("expected");QTest::addColumn<int>("rule");
        const auto add=[](const char* name,QStringList custody,QString currency,QString preference,QString session,bool available,QString expected,int rule){
            QTest::newRow(name)<<custody<<currency<<preference<<session<<available<<expected<<rule;
        };
        add("native-krw",{"phantom"},"KRW",{},{},true,"coin:upbit:KRW-SOL",3);
        add("native-usd",{"phantom"},"USD",{},{},true,"coin:binance:SOLUSDT",3);
        add("custody-before-currency",{"phantom","upbit"},"USD",{},{},true,"coin:upbit:KRW-SOL",2);
        add("public-before-custody",{"upbit"},"KRW","coin:binance:SOLUSDT",{},true,"coin:binance:SOLUSDT",1);
        add("session-before-public",{"upbit"},"KRW","coin:upbit:KRW-SOL","coin:binance:SOLUSDT",true,"coin:binance:SOLUSDT",0);
        add("catalog-failure-fallback",{"phantom"},"KRW",{},{},false,"coin:binance:SOLUSDT",4);
        add("bithumb-custody-supported-history",{"bithumb"},"KRW",{},{},true,"coin:upbit:KRW-SOL",3);
    }
    void canonicalSolChoiceUsesPublicThenCustodyThenCurrency(){
        QFETCH(QStringList,custody);QFETCH(QString,currency);QFETCH(QString,publicFirst);QFETCH(QString,session);
        QFETCH(bool,upbitAvailable);QFETCH(QString,expected);QFETCH(int,rule);
        auto markets=catalog();markets.upbitAvailable=upbitAvailable;HoldingMarketPreferences preferences;preferences.displayCurrency=currency;
        if(!publicFirst.isEmpty())preferences.publicPriority.append(WatchlistWorkspaceList::identityFromKey(publicFirst));
        if(!session.isEmpty())preferences.sessionSelections.insert("solana:native",WatchlistWorkspaceList::identityFromKey(session));
        const auto savedPriority=preferences.publicPriority;const auto savedSession=preferences.sessionSelections;
        const auto row=resolveHoldingMarket("solana:native",custody,markets,preferences);
        QCOMPARE(row.assetKey,QString("solana:native"));QVERIFY(row.chart);QCOMPARE(row.chart->key(),expected);
        QCOMPARE(row.selectionRule,rule);QVERIFY(row.candidates.contains(*row.chart));
        QCOMPARE(preferences.publicPriority,savedPriority);QCOMPARE(preferences.sessionSelections,savedSession);
        for(const auto& candidate:row.candidates)QVERIFY(candidate.source!="bithumb"); // No invented history support.
    }
    void publicStocksRequireExactVerifiedCountryAndAmbiguousLegacyHoldingsStayUnsupported(){
        HoldingMarketCatalog markets;markets.verifiedStocks={{"stock","KR","005930"},{"stock","US","AAPL"}};
        const auto kr=resolveHoldingMarket("stock:KR:005930",{},markets,{});QVERIFY(kr.chart);QCOMPARE(kr.chart->key(),QString("stock:KR:005930"));
        const auto us=resolveHoldingMarket("stock:US:AAPL",{},markets,{});QVERIFY(us.chart);QCOMPARE(us.chart->key(),QString("stock:US:AAPL"));
        QVERIFY(!resolveHoldingMarket("stock:KR:AAPL",{},markets,{}).chart); // Public country mismatch never falls back.
        const auto legacy=resolveHoldingMarket("stock:KR:AAPL",{"toss"},markets,{});QVERIFY(legacy.chart);QCOMPARE(legacy.chart->key(),QString("stock:US:AAPL"));
        markets.verifiedStocks.append({"stock","KR","ABCDEF"});markets.verifiedStocks.append({"stock","US","ABCDEF"});
        QVERIFY(!resolveHoldingMarket("stock:KR:ABCDEF",{"toss"},markets,{}).chart); // Legacy grouping has no country proof.
    }
    void rowsRetainUnsupportedAndStaleCustodyWithoutExpandingAlerts(){
        constexpr qint64 now=1800000000000;QList<AssetSnapshot> inputs;QList<PortfolioConnection> connections;
        const auto add=[&](QString provider,QString market,QString scope,QString id,QString asset,qint64 at,QString state="complete",AssetComponentKind kind=AssetComponentKind::Holding){
            AssetSnapshot snapshot;snapshot.provider=provider;snapshot.market=market;snapshot.environment=provider=="phantom"?"mainnet":"live";
            snapshot.accountScope=scope;snapshot.connectionId=scope;snapshot.timeMs=at;snapshot.balanceComplete=true;
            snapshot.evidence={"synthetic","https://example.invalid",scope,at,at,true};
            AssetComponent component;component.id=id;component.asset=asset;component.quantity="1";component.kind=kind;component.evidence=snapshot.evidence;
            component.displaySymbol="SOL";snapshot.components={component};inputs.append(snapshot);
            PortfolioConnection connection;connection.account.id=scope;connection.account.alias="NEVER_EXPOSE_ALIAS";connection.account.provider=provider;
            connection.account.market=market;connection.accountScope=scope;connection.state=state;connection.receivedAtMs=at;connections.append(connection);
        };
        add("phantom","solana","native","solana:native","SOL",now);
        add("upbit","spot","exchange","spot:SOL","SOL",now-600000);
        add("phantom","solana","mint","spl:other","solana:OTHER_MINT",now);
        add("phantom","solana","wrapped","spl:wrapped","solana:So11111111111111111111111111111111111111112",now);
        add("phantom","solana","lst","spl:lst","solana:LST_MINT",now,"failed");
        add("binance","spot","stale","spot:BTC","BTC",now-600000);
        add("binance","usdm_futures","position","position:SOL","SOL",now,"complete",AssetComponentKind::PositionNotional);
        auto markets=catalog();auto btc=markets.binance.first();btc.symbol="BTCUSDT";btc.baseAsset="BTC";markets.binance.append(btc);
        AssetValueRow value;value.accountScope="native";value.componentId="solana:native";value.asset="SOL";value.value="200000";value.included=true;
        value.priceEvidence={"PRIVATE_ARBITRARY_LABEL","https://api.upbit.com/v1/ticker","public:upbit:KRW-SOL",now,now,true};
        // Private scopes can use the same component id. Neither a different
        // scope's valuation nor a shared duplicate connection is authoritative.
        auto otherScope = value;
        otherScope.accountScope = "other-scope";
        otherScope.asset = "ETH";
        otherScope.priceEvidence.scope = "public:upbit:KRW-ETH";
        auto duplicate = connections.first();
        duplicate.state = "failed";
        connections.append(duplicate);
        duplicate.shared = true;
        connections.prepend(duplicate);
        const auto rows=resolveHoldingMarkets(inputs,connections,{otherScope,value,value},markets,{},now);QCOMPARE(rows.size(),5);
        const auto find=[&](QString key)->const HoldingChartRow*{for(const auto& row:rows)if(row.assetKey==key)return &row;return nullptr;};
        const auto* sol=find("solana:native");QVERIFY(sol);QCOMPARE(sol->custodies.size(),2);QVERIFY(sol->alertsEligible);
        QVERIFY(sol->chart);QCOMPARE(sol->chart->key(),QString("coin:upbit:KRW-SOL"));QCOMPARE(sol->valuationMarket,QString("업비트 KRW-SOL"));
        bool phantom=false,delayedUpbit=false;for(const auto& custody:sol->custodies){
            phantom|=custody.provider=="phantom"&&custody.label=="Phantom"&&custody.state==HoldingCustodyState::Fresh;
            delayedUpbit|=custody.provider=="upbit"&&custody.state==HoldingCustodyState::Delayed;
        }QVERIFY(phantom);QVERIFY(delayedUpbit);
        const auto* stale=find("exchange:BTC");QVERIFY(stale);QVERIFY(stale->chart);QVERIFY(!stale->alertsEligible);
        QCOMPARE(stale->custodies.first().state,HoldingCustodyState::Delayed);
        for(const auto& key:{QString("solana:OTHER_MINT"),QString("solana:So11111111111111111111111111111111111111112"),QString("solana:LST_MINT")}){
            const auto* separate=find(key);QVERIFY(separate);QVERIFY(!separate->chart);QVERIFY(!separate->chartReason.isEmpty());QVERIFY(!separate->alertsEligible);
        }
        QCOMPARE(find("solana:LST_MINT")->custodies.first().state,HoldingCustodyState::Failed);
        QVERIFY(!find("exchange:SOL"));
        for(const auto& row:rows){QVERIFY(!row.valuationMarket.contains("PRIVATE_ARBITRARY_LABEL"));for(const auto& custody:row.custodies)QVERIFY(!custody.label.contains("NEVER_EXPOSE_ALIAS"));}
    }
};
QTEST_GUILESS_MAIN(HoldingMarketResolverTest)
#include "tst_holding_market_resolver.moc"
