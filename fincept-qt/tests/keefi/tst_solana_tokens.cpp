#include "keefi/SolanaTokenData.h"
#include "keefi/SolanaReadRpc.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/PortfolioDetail.h"
#include <QDateTime>
#include <QJsonArray>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QBuffer>
#include <cstring>

using namespace keefi;
namespace {
class CountedRpcInput final : public QIODevice {
public:
    explicit CountedRpcInput(qint64 size,qint64 chunk=0):remaining_(size),chunk_(chunk){open(QIODevice::ReadOnly|QIODevice::Unbuffered);}
    explicit CountedRpcInput(const QByteArray& bytes,qint64 chunk=0):CountedRpcInput(qint64(bytes.size()),chunk){payload_=bytes;}
    bool isSequential() const override{return true;}
    qint64 bytesAvailable() const override{return remaining_+QIODevice::bytesAvailable();}
    qint64 consumed() const{return consumed_;}
    qint64 largestRequested() const{return largestRequested_;}
protected:
    qint64 readData(char* data,qint64 maximum) override{
        largestRequested_=qMax(largestRequested_,maximum);
        qint64 count=qMin(maximum,remaining_);if(chunk_>0)count=qMin(count,chunk_);
        if(count>0){
            if(payload_.isEmpty())std::memset(data,'R',size_t(count));
            else std::memcpy(data,payload_.constData()+consumed_,size_t(count));
        }
        remaining_-=count;consumed_+=count;return count;
    }
    qint64 writeData(const char*,qint64) override{return -1;}
private:
    qint64 remaining_=0,chunk_=0,consumed_=0,largestRequested_=0;
    QByteArray payload_;
};
const QString Skhy="SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3";
HttpResult json(const QJsonDocument& d){HttpResult h;h.status=200;h.body=d.toJson();return h;}
QJsonObject metadata(const QString& mint,const QString& symbol="USDC"){
    return {{"id",mint},{"symbol",symbol},{"name","USD Coin"},{"decimals",6},{"tokenProgram","TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"}};
}
AssetQuote quote(const QString& mint,const QString& value,qint64 at) {
    return {"solana:"+mint,"USD",value,{"Jupiter Price V3","https://api.jup.ag/price/v3?ids="+mint,"public:jupiter:price:"+mint,at,at,true}};
}
AssetSnapshot snapshot(qint64 at) {
    AssetSnapshot s;s.id="test";s.connectionId="wallet";s.accountScope="test";s.provider="phantom";s.market="solana";
    s.environment="mainnet";s.timeMs=at;s.balanceComplete=true;s.reconciled=true;
    s.evidence={"test","https://api.mainnet-beta.solana.com","test",at,at,true};
    for(const auto& mint:{UsdcMint,Skhy}){
        AssetComponent c;c.id=mint;c.asset="solana:"+mint;c.quantity="2";c.evidence=s.evidence;
        c.displaySymbol="USDC";c.displayName="Same symbol, different mint";s.components.append(c);
    }
    return s;
}
}
class SolanaTokensTest:public QObject {
    Q_OBJECT
private slots:
    void networkPolicyExplicitlyDisablesHttp2(){
        const auto rpc=SolanaReadRpc::request(SwapOperation::Genesis,"synthetic-policy",{});
        auto metadata=SolanaTokenData::request(true,{UsdcMint});
        metadata.headers.insert("x-api-key","SYNTHETIC_POLICY_KEY");
        for(const auto& request:{rpc,metadata,SolanaTokenData::request(false,{UsdcMint})}){
            QVERIFY(NetworkSwapTransport::allowed(request));
            const auto policy=NetworkSwapTransport::requestPolicy(request);
            const auto http2=policy.attribute(QNetworkRequest::Http2AllowedAttribute);
            QVERIFY2(http2.isValid()&&!http2.toBool(),"Solana/Jupiter request policy must explicitly disable HTTP/2");
            QCOMPARE(policy.url(),request.url);
            QCOMPARE(policy.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),int(QNetworkRequest::ManualRedirectPolicy));
            QCOMPARE(policy.rawHeader("Accept"),QByteArray("application/json"));
            for(auto it=request.headers.constBegin();it!=request.headers.constEnd();++it)
                QCOMPARE(policy.rawHeader(it.key()),it.value());
        }
    }
    void responseReadLimitsSmallJsonRequestsToAvailableBytes(){
        const QByteArray payload("{\"jsonrpc\":\"2.0\",\"id\":\"synthetic-small-read\",\"result\":7}");
        QVERIFY(!payload.isEmpty());QVERIFY(payload.size()<64*1024);
        CountedRpcInput input(payload,3);QByteArray bytes;
        QVERIFY(NetworkSwapTransport::readResponseChunk(input,bytes));
        QCOMPARE(input.consumed(),qint64(payload.size()));QCOMPARE(bytes,payload);
        QVERIFY(input.largestRequested()>0);QVERIFY(input.largestRequested()<=qint64(payload.size()));
        HttpResult h;h.status=200;h.body=bytes;
        const auto parsed=SolanaReadRpc::parse(SwapOperation::BlockHeight,h,"synthetic-small-read");
        QVERIFY(parsed.ok);QCOMPARE(parsed.value.toString(),QString("7"));
    }
    void responseReadUsesRemainingBudgetAndDoesNotReadAgainAfterOversize(){
        constexpr qsizetype limit=4*1024*1024;QByteArray bytes(limit-3,'B');CountedRpcInput input(32);
        QVERIFY(!NetworkSwapTransport::readResponseChunk(input,bytes));QCOMPARE(input.consumed(),qint64(4));
        QVERIFY(input.largestRequested()<=qint64(4));
        QCOMPARE(bytes.size(),limit+1);QCOMPARE(bytes.left(8),QByteArray(8,'B'));
        QVERIFY(!NetworkSwapTransport::readResponseChunk(input,bytes));QCOMPARE(input.consumed(),qint64(4));
    }
    void responseReadHandlesExactLimitAndShortChunks(){
        constexpr qsizetype limit=4*1024*1024;QByteArray bytes(limit-8,'B');CountedRpcInput input(8,3);
        QVERIFY(NetworkSwapTransport::readResponseChunk(input,bytes));QCOMPARE(input.consumed(),qint64(8));
        QVERIFY(input.largestRequested()<=qint64(8));
        QCOMPARE(bytes.size(),limit);QCOMPARE(bytes.right(8),QByteArray(8,'R'));
        CountedRpcInput final(1);QVERIFY(!NetworkSwapTransport::readResponseChunk(final,bytes));
        QCOMPARE(final.consumed(),qint64(1));QCOMPARE(bytes.size(),limit+1);
        QVERIFY(final.largestRequested()<=qint64(1));
    }
    void responseReadPreservesSmallJsonAcrossCallbacks(){
        QByteArray first("{\"jsonrpc\":\"2.0\",\"id\":\"synthetic-read\",");
        QByteArray last("\"result\":7}");QBuffer a(&first),b(&last);QVERIFY(a.open(QIODevice::ReadOnly));QVERIFY(b.open(QIODevice::ReadOnly));
        QByteArray bytes;QVERIFY(NetworkSwapTransport::readResponseChunk(a,bytes));QVERIFY(NetworkSwapTransport::readResponseChunk(b,bytes));
        QCOMPARE(bytes,first+last);HttpResult h;h.status=200;h.body=bytes;
        const auto parsed=SolanaReadRpc::parse(SwapOperation::BlockHeight,h,"synthetic-read");QVERIFY(parsed.ok);QCOMPARE(parsed.value.toString(),QString("7"));
    }
    void responseReadStopsAtExistingBudgetBeforeCopyingWholeChunk(){
        constexpr qint64 limit=4*1024*1024;CountedRpcInput input(limit+8192);QByteArray bytes;
        QVERIFY(!NetworkSwapTransport::readResponseChunk(input,bytes));
        QVERIFY2(input.consumed()<=limit+1,"RPC input consumption must stop at the existing 4 MiB limit plus one sentinel byte");
        QCOMPARE(bytes.size(),qsizetype(limit+1));
    }
    void exactMintMetadataAndIndependentFailures(){
        QString error;
        auto h=json(QJsonDocument(QJsonArray{metadata(UsdcMint),metadata(Skhy,"SKHY")}));
        const auto names=SolanaTokenData::metadata(h,{UsdcMint,Skhy},&error);
        QVERIFY(error.isEmpty());QCOMPARE(names.size(),2);QCOMPARE(names[1].mint,Skhy);QCOMPARE(names[1].symbol,QString("SKHY"));
        // Searching an exact mint never accepts a symbol-only match or another mint.
        QCOMPARE(SolanaTokenData::metadata(h,{SolMint},&error).size(),0);
        h=json(QJsonDocument(QJsonArray{metadata(UsdcMint),metadata(UsdcMint)}));
        QVERIFY(SolanaTokenData::metadata(h,{UsdcMint},&error).isEmpty());QVERIFY(!error.isEmpty());
        h=json(QJsonDocument(QJsonArray{metadata(UsdcMint,QString("USDC\u202e"))}));
        QVERIFY(SolanaTokenData::metadata(h,{UsdcMint},&error).isEmpty());
        h.status=503;QVERIFY(SolanaTokenData::metadata(h,{UsdcMint},&error).isEmpty());QCOMPARE(error,QString("HTTP 503"));
    }
    void missingAndZeroPricesNeverCreateQuotes(){
        QString error;
        const auto row=[](const QJsonValue& price){return QJsonObject{{"usdPrice",price},{"blockId",12345},{"decimals",6}};};
        auto h=json(QJsonDocument(QJsonObject{{UsdcMint,row(0)},{Skhy,row(QJsonValue::Null)},{SolMint,row("10")}}));
        QVERIFY(SolanaTokenData::prices(h,{UsdcMint,Skhy},&error).isEmpty());
        h=json(QJsonDocument(QJsonObject{{UsdcMint,row("0.9999342445371535")},{Skhy,row("135.72823415438393")}}));
        const auto prices=SolanaTokenData::prices(h,{UsdcMint,Skhy},&error);
        QCOMPARE(prices.size(),2);QCOMPARE(prices.first().usd,QString("0.9999342445371535"));
    }
    void allowlistSendsOnlyPublicMints(){
        for(bool metadata:{false,true}){
            const auto request=SolanaTokenData::request(metadata,{UsdcMint,Skhy});
            QVERIFY(NetworkSwapTransport::allowed(request));QVERIFY(request.body.isEmpty());QVERIFY(request.headers.isEmpty());
            auto bad=request;bad.url.setHost("example.com");QVERIFY(!NetworkSwapTransport::allowed(bad));
            QVERIFY(!NetworkSwapTransport::allowed(SolanaTokenData::request(metadata,{"USDC"})));
            QVERIFY(!NetworkSwapTransport::allowed(SolanaTokenData::request(metadata,{UsdcMint,UsdcMint})));
        }
        QVERIFY(NetworkSwapTransport::allowed(SolanaTokenData::blockTimesRequest("id",{"12345","12346"})));
        auto wrong=SolanaTokenData::blockTimesRequest("id",{"12345"});wrong.body.replace("getBlockTime","sendTransaction");
        QVERIFY(!NetworkSwapTransport::allowed(wrong));
    }
    void token2022ScaledUnitsMustMatchPriceUnits(){
        const QString program="TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb";
        const auto account=[&](const QString& extension,const QString& multiplier){
            QJsonObject state{{"multiplier",multiplier},{"newMultiplier",multiplier}};
            const QJsonObject info{{"decimals","6"},{"isInitialized",true},{"extensions",QJsonArray{QJsonObject{{"extension",extension},{"state",state}}}}};
            return QJsonObject{{"owner",program},{"executable",false},{"data",QJsonObject{{"parsed",QJsonObject{{"type","mint"},{"info",info}}}}}};
        };
        QVERIFY(SolanaTokenData::supportsBaseUnits(account("scaledUiAmountConfig","1"),program,6));
        QVERIFY(!SolanaTokenData::supportsBaseUnits(account("scaledUiAmountConfig","2"),program,6));
        QVERIFY(!SolanaTokenData::supportsBaseUnits(account("interestBearingConfig","1"),program,6));
        QVERIFY(!SolanaTokenData::supportsBaseUnits(account("scaledUiAmountConfig","1"),program,9));
        QVERIFY(!SolanaTokenData::supportsBaseUnits(account("scaledUiAmountConfig","1"),"wrong-program",6));
    }
    void sourceTimeUsesCorrelatedBlockResponse(){
        const auto seconds=QDateTime::currentSecsSinceEpoch()-5;QString error;
        const auto h=json(QJsonDocument(QJsonArray{
            QJsonObject{{"jsonrpc","2.0"},{"id","request:456"},{"result",QString::number(seconds)}},
            QJsonObject{{"jsonrpc","2.0"},{"id","other:123"},{"result",QString::number(seconds)}},
            QJsonObject{{"jsonrpc","2.0"},{"id","request:123"},{"result",QJsonValue::Null}}}));
        const auto times=SolanaTokenData::blockTimes(h,"request",{"123","456"},&error);
        QCOMPARE(times.size(),1);QCOMPARE(times.value("456"),seconds*1000);QVERIFY(!times.contains("123"));
    }
    void mintIdentityAndKrwConversion(){
        const auto at=QDateTime::currentMSecsSinceEpoch()-1000;auto s=snapshot(at);
        const AssetQuote krw{"USDC","KRW","1400",{"Upbit","https://api.upbit.com/v1/ticker","public:upbit:KRW-USDC",at,at,true}};
        QList<AssetQuote> prices{quote(UsdcMint,"0.98",at),quote(Skhy,"137.2",at),krw};
        const auto result=AssetValuation::value({s},prices,"KRW",at);
        QVERIFY(result.complete);QCOMPARE(result.rows[0].value,QString("2800"));QCOMPARE(result.rows[1].value,QString("392000"));
        QCOMPARE(result.rows[1].conversionEvidence.size(),3);
        const auto groups=presentPortfolio({s},result,{}).groups;QCOMPARE(groups.size(),2);
        QVERIFY(groups[0].identity!=groups[1].identity);
        QCOMPARE(portfolioAssetLabel(groups[0].asset,groups[0].displaySymbol),QString("USDC"));
        // A symbol-matched exchange price cannot value a different mint.
        const AssetQuote ticker{"SKHY","KRW","999999",krw.evidence};
        const auto unpriced=AssetValuation::value({s},{ticker,krw},"KRW",at);
        QVERIFY(unpriced.rows[0].value.isEmpty());QVERIFY(unpriced.rows[1].value.isEmpty());
        prices[0].evidence.sourceTimeMs=at-DisplayFreshnessMs-1;
        QVERIFY(AssetValuation::value({s},prices,"KRW",at,DisplayFreshnessMs).rows[1].value.isEmpty());
        s.components[0].quantity="0";
        const auto zero=AssetValuation::value({s},{},"KRW",at);
        QCOMPARE(zero.rows[0].value,QString("0"));QVERIFY(zero.rows[1].value.isEmpty());
        const auto view=presentPortfolio({s},zero,{});
        QCOMPARE(view.summary.unvaluedGroups,1);
        QCOMPARE(solanaPriceRatio("1","3"),QString("0.333333333333333333333333"));
        QCOMPARE(solanaPriceRatio("0.01","0.02"),QString("0.5"));
        QVERIFY(solanaPriceRatio("1","0").isEmpty());
    }
    void thinUpbitUsdcFallsBackToFresherBinanceLeg(){
        const auto at=QDateTime::currentMSecsSinceEpoch()-1000;const auto s=snapshot(at);
        // T1 (2026-10-20): Upbit KRW-USDC was 426 s old while Jupiter, Binance USDC/USDT and KRW-USDT were fresh.
        const AssetQuote staleUpbit{"USDC","KRW","1338",{"Upbit","https://api.upbit.com/v1/ticker","public:upbit:KRW-USDC",at-426000,at,true}};
        const AssetQuote binance{"USDC","USDT","1.001",{"Binance","wss://stream.binance.com:9443/ws","public:binance:USDCUSDT",at,at,true}};
        const AssetQuote usdt{"USDT","KRW","1336",{"Upbit","https://api.upbit.com/v1/ticker","public:upbit:KRW-USDT",at,at,true}};
        const QList<AssetQuote> prices{quote(UsdcMint,"1",at),quote(Skhy,"140",at),staleUpbit,binance,usdt};
        const auto result=AssetValuation::value({s},prices,"KRW",at,DisplayFreshnessMs);
        QVERIFY(result.complete);
        QCOMPARE(result.rows[1].value,QString("374454.08")); // 2 × 140 × 1.001 × 1336
        QCOMPARE(result.rows[1].conversionEvidence.size(),4);
        QVERIFY(result.rows[1].priceEvidence.source.contains("Binance"));
        // A fresh Upbit KRW-USDC at least as recent keeps the direct leg.
        auto fresh=prices;fresh[2].evidence.sourceTimeMs=at;fresh[2].price="1340";
        const auto direct=AssetValuation::value({s},fresh,"KRW",at,DisplayFreshnessMs);
        QCOMPARE(direct.rows[1].value,QString("375200"));QCOMPARE(direct.rows[1].conversionEvidence.size(),3);
        // Without the Binance legs a stale Upbit leg still cannot make the value current.
        const auto alone=AssetValuation::value({s},{prices[0],prices[1],staleUpbit},"KRW",at,DisplayFreshnessMs);
        QVERIFY(alone.rows[1].value.isEmpty());
    }
    void livePublicMints(){
        if(!qEnvironmentVariableIsSet("KEEFI_VERIFY_PUBLIC_MINTS"))QSKIP("Opt-in public mint verification");
        NetworkSwapTransport transport;QSignalSpy spy(&transport,&SwapTransport::finished);QString error;
        const QStringList mints{UsdcMint,Skhy};
        transport.send(1,SolanaTokenData::request(true,mints));QVERIFY(spy.wait(21000));
        const auto names=SolanaTokenData::metadata(qvariant_cast<HttpResult>(spy.takeFirst()[1]),mints,&error);
        QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(names.size(),2);
        QMap<QString,QString> symbols;for(const auto& name:names)symbols[name.mint]=name.symbol;
        QCOMPARE(symbols.value(UsdcMint),QString("USDC"));QCOMPARE(symbols.value(Skhy),QString("SKHY"));
        QJsonArray m;for(const auto& mint:mints)m.append(mint);
        transport.send(4,SolanaReadRpc::request(SwapOperation::Accounts,"mint-types",{m,QJsonObject{{"encoding","jsonParsed"},{"commitment","confirmed"}}}));
        QVERIFY(spy.wait(21000));const auto mintResult=SolanaReadRpc::parse(SwapOperation::Accounts,qvariant_cast<HttpResult>(spy.takeFirst()[1]),"mint-types");
        QVERIFY(mintResult.ok);const auto mintAccounts=mintResult.value.toObject().value("value").toArray();QCOMPARE(mintAccounts.size(),mints.size());
        for(int i=0;i<mints.size();++i){const auto found=std::find_if(names.cbegin(),names.cend(),[&](const auto& n){return n.mint==mints[i];});
            QVERIFY(found!=names.cend());QVERIFY(SolanaTokenData::supportsBaseUnits(mintAccounts[i],found->program,found->decimals));}
        transport.send(2,SolanaTokenData::request(false,mints));QVERIFY(spy.wait(21000));
        const auto prices=SolanaTokenData::prices(qvariant_cast<HttpResult>(spy.takeFirst()[1]),mints,&error);
        QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(prices.size(),2);
        QStringList blocks;for(const auto& price:prices)if(!blocks.contains(price.block))blocks.append(price.block);
        transport.send(3,SolanaTokenData::blockTimesRequest("live",blocks));QVERIFY(spy.wait(21000));
        const auto times=SolanaTokenData::blockTimes(qvariant_cast<HttpResult>(spy.takeFirst()[1]),"live",blocks,&error);
        QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(times.size(),blocks.size());
        for(const auto& price:prices)QVERIFY(QDateTime::currentMSecsSinceEpoch()-times.value(price.block)<DisplayFreshnessMs);
    }
};
QTEST_MAIN(SolanaTokensTest)
#include "tst_solana_tokens.moc"
