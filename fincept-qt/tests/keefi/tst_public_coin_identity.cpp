#include "keefi/PublicCoinIdentity.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
using namespace keefi;
class PublicCoinIdentityTest final:public QObject {
    Q_OBJECT
private slots:
    void pinnedSymbolNeedsItsCoinGeckoIdWhateverTheNames(){
        PublicMarketRow market;market.market="KRW-SOL";market.base="SOL";market.englishName="Solana";
        PublicBoardItem coin;coin.symbol="SOL";coin.name="Solana";coin.sourceId="solana";coin.marketCap=100;coin.currency="KRW";
        QVERIFY(samePublicCoin(market,coin));QVERIFY(verifiedCoinMarketCap("SOL",{market},{coin}).has_value());
        // The pinned id decides: a copy reusing both symbol and name is rejected.
        auto other=coin;other.sourceId="synthetic-sol";QVERIFY(!samePublicCoin(market,other));
        other=coin;other.name="Solana Copy";other.sourceId="solana-copy";QVERIFY(!samePublicCoin(market,other));
        // Exchange naming never splits a pinned coin (public lists, 2026-11-07).
        PublicMarketRow ada;ada.market="KRW-ADA";ada.base="ADA";ada.englishName="Ada";
        PublicBoardItem cardano;cardano.symbol="ADA";cardano.name="Cardano";cardano.sourceId="cardano";cardano.marketCap=100;cardano.currency="KRW";
        QVERIFY(samePublicCoin(ada,cardano));QVERIFY(verifiedCoinMarketCap("ADA",{ada},{cardano}).has_value());
        PublicMarketRow atom;atom.market="KRW-ATOM";atom.base="ATOM";atom.englishName="Cosmos";
        PublicBoardItem hub;hub.symbol="ATOM";hub.name="Cosmos Hub";hub.sourceId="cosmos";QVERIFY(samePublicCoin(atom,hub));
        hub.sourceId="cosmos-copy";QVERIFY(!samePublicCoin(atom,hub));
        other=coin;other.name.clear();QVERIFY(!samePublicCoin(market,other));
        market.englishName.clear();QVERIFY(!verifiedCoinMarketCap("SOL",{market},{coin}));
        market.englishName="Solana";other=coin;other.sourceId="synthetic-sol";
        QVERIFY(!verifiedCoinMarketCap("SOL",{market},{other}));
        QCOMPARE(verifiedCoinMarketCap("SOL",{market},{other,coin})->sourceId,QString("solana"));
        QVERIFY(!verifiedCoinMarketCap("SOL",{}, {coin}));
    }
    void unpinnedCoinJoinsThroughTheExchangeEnglishName(){
        // Most Upbit KRW coins are not in the pinned id table. Their exact
        // English name, not the symbol alone, establishes the public join.
        PublicMarketRow market;market.market="KRW-PEPE";market.base="PEPE";market.englishName="Pepe";
        PublicBoardItem coin;coin.symbol="PEPE";coin.name="Pepe";coin.sourceId="pepe";coin.marketCap=100;coin.currency="KRW";
        QVERIFY(samePublicCoin(market,coin));QCOMPARE(verifiedCoinMarketCap("PEPE",{market},{coin})->sourceId,QString("pepe"));
        auto spaced=coin;spaced.name=QStringLiteral(" PEPE ");QVERIFY(samePublicCoin(market,spaced));
        auto other=coin;other.name="Pepe Unchained";QVERIFY(!samePublicCoin(market,other));
        other=coin;other.symbol="PEPE2";QVERIFY(!samePublicCoin(market,other));
        other=coin;other.name.clear();QVERIFY(!samePublicCoin(market,other));
        auto copy=coin;copy.sourceId="pepe-copy";QVERIFY(!verifiedCoinMarketCap("PEPE",{market},{coin,copy}));
        auto usd=coin;usd.currency="USD";QVERIFY(!verifiedCoinMarketCap("PEPE",{market},{usd}));
    }
    void parsedDuplicateSymbolsNeverEstablishAnUnpinnedJoin_data(){
        QTest::addColumn<int>("otherCoins");
        QTest::newRow("adjacent-duplicates")<<0;
        QTest::newRow("duplicate-after-display-limit")<<199;
    }
    void parsedDuplicateSymbolsNeverEstablishAnUnpinnedJoin(){
        QFETCH(int,otherCoins);
        const auto quote=[](const QString& symbol,const QString& id,const QString& name){
            return QJsonObject{{"symbol",symbol},{"id",id},{"name",name},{"market_cap",1000.0},{"current_price",1.0}};
        };
        QJsonArray response{quote("pepe","pepe","Pepe")};
        for(int i=0;i<otherCoins;++i)response.append(quote("c"+QString::number(i),"coin-"+QString::number(i),"Coin "+QString::number(i)));
        response.append(quote("pepe","pepe-copy","Pepe"));
        QList<PublicBoardItem> parsed;QVERIFY(PublicEquityBoard::parseCoins(QJsonDocument(response).toJson(),&parsed));
        QCOMPARE(parsed.size(),otherCoins+1); // The public map still has one row per symbol.
        PublicMarketRow market;market.market="KRW-PEPE";market.base="PEPE";market.englishName="Pepe";
        QVERIFY2(!verifiedCoinMarketCap("PEPE",{market},parsed),
            "Display deduplication must not erase evidence of conflicting CoinGecko identities");
    }
    void parsedPinnedCoinStillUsesItsExactId(){
        const QByteArray response=R"([{"id":"solana","symbol":"sol","name":"Solana","market_cap":1000,"current_price":1},{"id":"solana-copy","symbol":"sol","name":"Solana Copy","market_cap":100,"current_price":1}])";
        QList<PublicBoardItem> parsed;QVERIFY(PublicEquityBoard::parseCoins(response,&parsed));QCOMPARE(parsed.size(),1);
        PublicMarketRow market;market.market="KRW-SOL";market.base="SOL";market.englishName="Solana";
        const auto joined=verifiedCoinMarketCap("SOL",{market},parsed);QVERIFY(joined);QCOMPARE(joined->sourceId,QString("solana"));
    }
};
QTEST_GUILESS_MAIN(PublicCoinIdentityTest)
#include "tst_public_coin_identity.moc"
