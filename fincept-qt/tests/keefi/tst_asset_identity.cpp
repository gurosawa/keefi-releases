#include "keefi/PortfolioPresentation.h"
#include "keefi/AssetIdentity.h"
#include <QTest>
using namespace keefi;

class AssetIdentityTest final:public QObject {
    Q_OBJECT
private slots:
    void registryRequiresCanonicalComponentAndLedger_data(){
        QTest::addColumn<QString>("provider");QTest::addColumn<QString>("market");QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("asset");QTest::addColumn<QString>("ledger");QTest::addColumn<int>("kind");QTest::addColumn<QString>("expected");
        const auto add=[](const char* name,QString provider,QString market,QString id,QString asset,QString ledger,AssetComponentKind kind,QString expected){
            QTest::newRow(name)<<provider<<market<<id<<asset<<ledger<<int(kind)<<expected;
        };
        add("native","phantom","solana","solana:native","SOL",{},AssetComponentKind::Holding,"solana:native");
        add("spoof-native-id","phantom","solana","other","SOL",{},AssetComponentKind::Holding,"solana:unresolved:other");
        add("unknown-wallet-provider","other","solana","solana:native","SOL",{},AssetComponentKind::Holding,"solana:unresolved:solana:native");
        add("same-display-symbol-mint","phantom","solana","spl:mint","solana:mint",{},AssetComponentKind::Holding,"solana:mint");
        add("wrapped-sol","phantom","solana","spl:wrapped","solana:So11111111111111111111111111111111111111112",{},AssetComponentKind::Holding,"solana:So11111111111111111111111111111111111111112");
        for(const auto* provider:{"upbit","binance","bithumb","okx"})add(provider,provider,"spot","spot:SOL","SOL","spot",AssetComponentKind::Holding,"solana:native");
        add("earn-ledger","binance","earn","earn:SOL","SOL","earn",AssetComponentKind::Holding,"solana:native");
        add("unknown-exchange","other","spot","spot:SOL","SOL","spot",AssetComponentKind::Holding,"exchange:SOL");
        add("position","binance","usdm_futures","position:SOL","SOL","positions",AssetComponentKind::PositionNotional,"exchange:SOL");
        add("okx-funding","okx","okx_unified","funding:SOL","SOL","funding",AssetComponentKind::Holding,"solana:native");
        add("okx-trading-equity","okx","okx_unified","trading:SOL","SOL","trading",AssetComponentKind::Equity,"exchange:SOL");
        add("okx-funding-equity","okx","okx_unified","funding:SOL","SOL","funding",AssetComponentKind::Equity,"exchange:SOL");
        add("okx-wrong-ledger","okx","okx_unified","trading:SOL","SOL","trading",AssetComponentKind::Holding,"exchange:SOL");
    }
    void registryRequiresCanonicalComponentAndLedger(){
        QFETCH(QString,provider);QFETCH(QString,market);QFETCH(QString,id);QFETCH(QString,asset);QFETCH(QString,ledger);QFETCH(int,kind);QFETCH(QString,expected);
        AssetSnapshot snapshot;snapshot.provider=provider;snapshot.market=market;AssetComponent component;
        component.id=id;component.asset=asset;component.valuationGroup=ledger;component.kind=AssetComponentKind(kind);component.displaySymbol="SOL";
        QCOMPARE(assetIdentity(snapshot,component),expected);QCOMPARE(AssetIdentityRegistryVersion,1);
    }
    void nativeAndExchangeSolMergeWithoutChangingValuationOrMintRows(){
        constexpr qint64 now=1800000000000;
        const auto snapshot=[&](QString provider,QString market,QString scope,QString id,QString asset,QString quantity,AssetComponentKind kind=AssetComponentKind::Holding){
            AssetSnapshot value;value.provider=provider;value.market=market;value.accountScope=scope;value.connectionId=scope;
            value.timeMs=now;value.balanceComplete=true;value.evidence={"synthetic","https://example.invalid",scope,now,now,true};
            AssetComponent component;component.id=id;component.asset=asset;component.quantity=quantity;component.kind=kind;component.evidence=value.evidence;
            value.components={component};return value;
        };
        const auto native=snapshot("phantom","solana","wallet","solana:native","SOL","5");
        const auto exchange=snapshot("upbit","spot","exchange","SOL","SOL","3");
        auto mint=snapshot("phantom","solana","mint","spl:FAKE_MINT","solana:FAKE_MINT","2");
        mint.components[0].displaySymbol="SOL"; // A display alias cannot establish asset equivalence.
        const auto position=snapshot("binance","usdsm","futures","SOLUSDT:position","SOL","999",AssetComponentKind::PositionNotional);
        const QList<AssetSnapshot> inputs{native,exchange,mint,position};const auto untouched=inputs;
        AssetValuationResult valuation;valuation.currency="KRW";valuation.timeMs=now;valuation.complete=true;
        valuation.netAssets="1614";valuation.knownSubtotal="1614";
        for(const auto& pair:QList<QPair<AssetSnapshot,QString>>{{native,"1000"},{exchange,"600"},{mint,"14"},{position,{}}}){
            const auto& component=pair.first.components.first();AssetValueRow row;row.accountScope=pair.first.accountScope;
            row.componentId=component.id;row.asset=component.asset;row.quantity=component.quantity;row.value=pair.second;
            row.currency="KRW";row.included=!pair.second.isEmpty();valuation.rows.append(row);
        }
        const auto originalRows=valuation.rows;
        const auto view=presentPortfolio(inputs,valuation,{});
        const PortfolioGroupView* sol=nullptr;const PortfolioGroupView* separateMint=nullptr;
        for(const auto& group:view.groups){if(group.identity=="solana:native")sol=&group;if(group.identity=="solana:FAKE_MINT")separateMint=&group;}
        QVERIFY(sol);QCOMPARE(sol->holdings.size(),2);QCOMPARE(portfolioQuantity(*sol),QString("8"));
        QCOMPARE(sol->freshTotal,QString("1600"));QCOMPARE(sol->storages.size(),2);
        QVERIFY(separateMint);QCOMPARE(separateMint->holdings.size(),1);QCOMPARE(portfolioQuantity(*separateMint),QString("2"));
        QCOMPARE(view.summary.netAssets,QString("1614"));QCOMPARE(view.summary.knownSubtotal,QString("1614"));
        QCOMPARE(portfolioAllocationTotal(view.groups),QString("1614"));QCOMPARE(inputs,untouched);
        QCOMPARE(valuation.rows.size(),originalRows.size());
        for(int i=0;i<valuation.rows.size();++i){QCOMPARE(valuation.rows[i].value,originalRows[i].value);QCOMPARE(valuation.rows[i].quantity,originalRows[i].quantity);}
        QVERIFY(!portfolioGroupLabel(*sol).contains(QStringLiteral("지갑"))); // Mixed custody is not a wallet-only label.
    }
};
QTEST_GUILESS_MAIN(AssetIdentityTest)
#include "tst_asset_identity.moc"
