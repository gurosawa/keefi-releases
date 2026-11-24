#include "keefi/BinanceEarnProtocol.h"
#include "keefi/EarnTracking.h"
#include "keefi/ResponseJournal.h"
#include "keefi/TradingTransport.h"
#include "keefi/BinanceEarnCollector.h"
#include "keefi/AssetsLedger.h"
#include "keefi/Vault.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <sodium.h>
using namespace keefi;
namespace {
constexpr qint64 Now=1793448000000LL;
constexpr qint64 Day=86400000LL;
CredentialSet credentials() {
    CredentialSet c;
    const QByteArray key="SyntheticEarnKey123",secret="SyntheticEarnSecret123";
    c.emplace("api_key",SecureBuffer(key.constData(),std::size_t(key.size())));
    c.emplace("secret",SecureBuffer(secret.constData(),std::size_t(secret.size()))); return c;
}
EarnQuery query(EarnSource source,int page=1,int size=100) {return {source,earnPositions(source)?0:Now-86400000,earnPositions(source)?0:Now,page,size};}
HttpResult response(const QJsonArray& rows,int total=-1) {
    HttpResult h;h.status=200;h.body=QJsonDocument(QJsonObject{{"rows",rows},{"total",total<0?int(rows.size()):total}}).toJson(QJsonDocument::Compact);return h;
}
QJsonObject subscription(QString id="9007199254740993",QString status="SUCCESS") {
    return {{"amount","100"},{"asset","SYN"},{"time",QString::number(Now-60000)},
        {"purchaseId",id},{"productId","SYN001"},{"type","AUTO"},{"sourceAccount","SPOT"},
        {"amtFromSpot","30"},{"amtFromFunding","70"},{"status",status}};
}
QJsonObject position() {return {{"totalAmount","103"},{"asset","SYN"},{"productId","SYN001"},{"autoSubscribe",true},{"collateralAmount","0"},{"cumulativeTotalRewards","99"}};}
EarnAccountState account() {EarnAccountState s;s.accountScope=QString(64,'a');s.binding=QString(64,'b');s.connectionId="synthetic-earn";s.trackingStartMs=Now-86400000;return s;}
EarnAccountState queriedAccount() {
    auto s=account();s.trackingStartMs=Now-400*Day;s.revision=1;
    for(const auto source:earnSources())if(!earnPositions(source))
        s.windows.append({source,s.trackingStartMs-30*Day+1,Now,Now});
    return s;
}
EarnRecord storedObservation(EarnSource source,const QString& status,qint64 at,QStringList issues={}) {
    EarnRecord r;r.source=source;r.id=QString(64,'c');r.productId="SYN001";r.asset="SYN";
    r.amount="100";r.timeMs=at;r.receivedAtMs=Now;r.status=status;r.issues=std::move(issues);return r;
}
QList<EarnQuery> sourceQueries(const EarnAccountState& s,EarnSource source,qint64 now=Now) {
    QList<EarnQuery> found;for(const auto& q:EarnTracking::queries(s,now))if(q.source==source)found.append(q);return found;
}
EarnBatch parsed(EarnSource source,const QJsonArray& rows) {
    const auto q=query(source);EarnPageAccumulator a;
    if(!a.add(q,BinanceEarnProtocol::parsePage(q,response(rows),Now),Now))return {};
    return a.batch();
}
const QString Password="Synthetic Earn Password! 2026";
bool prepare(Vault& v){AccountRecord a;a.provider="binance";a.environment="live";a.market="spot";a.alias="synthetic";a.currency="USDT";return v.create(Password)&&v.addAccount(a,{{"api_key","SyntheticEarnKey123"},{"secret","SyntheticEarnSecret123"}},StorageMode::EncryptedVault);}
AssetSnapshot snapshot(Vault& v){
    const auto a=v.accounts().first();AssetSnapshot s;s.id="synthetic-snapshot";s.accountScope=QString(64,'a');s.connectionId=a.id;s.provider=a.provider;s.environment=a.environment;s.market=a.market;s.timeMs=Now;s.balanceComplete=s.reconciled=true;s.evidence={"synthetic official","https://api.binance.com/api/v3/account",s.accountScope,0,Now,true};
    QCryptographicHash h(QCryptographicHash::Sha256);h.addData(QByteArrayView("KEEFI/current-assets/credential-binding/v1"));
    for(const auto& part:{a.provider,a.environment,a.market,a.region}){const auto b=part.toUtf8();h.addData(QByteArray::number(b.size()));h.addData(":");h.addData(b);}
    v.withCredentials(a.id,[&](const CredentialSet& c){for(const auto& [name,value]:c){h.addData(name.toUtf8());h.addData(":");h.addData(QByteArray::number(value.size()));h.addData(":");h.addData(QByteArrayView(reinterpret_cast<const char*>(value.data()),qsizetype(value.size())));}});
    s.connectionBinding=QString::fromLatin1(h.result().toHex());return s;
}
class EarnWire final:public TradingTransport {
public:
    QList<RequestSpec> requests;std::function<void()> onSend;bool delayed=false;quint64 pending=0;RequestSpec request;qint64 serverNow=Now;
    using TradingTransport::TradingTransport;
    void abort()override{pending=0;}
    void send(quint64 id,const RequestSpec& r)override{requests.append(r);pending=id;request=r;if(onSend)onSend();if(!delayed)deliver();}
    void deliver(){if(!pending)return;const auto id=pending;pending=0;HttpResult h;
        if(request.operation==Operation::Time){h.status=200;h.body=QJsonDocument(QJsonObject{{"serverTime",QString::number(serverNow)}}).toJson(QJsonDocument::Compact);}
        else if(request.url.path()==earnPath(EarnSource::FlexibleSubscriptions))h=response({subscription()});
        else if(request.url.path()==earnPath(EarnSource::FlexiblePositions))h=response({position()});
        else h=response({});
        emit finished(id,h);
    }
};
}
class BinanceEarnTest final:public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){QVERIFY(sodium_init()>=0);}
    void exactReadOnlyAuthority_data(){QTest::addColumn<int>("source");for(const auto s:earnSources())QTest::newRow(qPrintable(earnSourceToken(s)))<<int(s);}
    void exactReadOnlyAuthority(){
        QFETCH(int,source);const auto q=query(EarnSource(source));auto c=credentials();QString error;
        const auto r=BinanceEarnProtocol::request(q,c,Now,&error);QVERIFY2(error.isEmpty(),qPrintable(error));
        QVERIFY(allowedEarnRead(r));QVERIFY(NetworkTradingTransport::allowed(r));QCOMPARE(r.method,QByteArray("GET"));QVERIFY(r.body.isEmpty());
        QVERIFY(!r.url.toString().contains("SyntheticEarnSecret"));
        for(const auto& method:{QByteArray("POST"),QByteArray("DELETE"),QByteArray("PUT")}){auto bad=r;bad.method=method;QVERIFY(!NetworkTradingTransport::allowed(bad));}
        auto bad=r;bad.url.setPath("/sapi/v1/simple-earn/flexible/subscribe");QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;auto params=QUrlQuery(bad.url);params.addQueryItem("amount","1");bad.url.setQuery(params);QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;params=QUrlQuery(bad.url);params.addQueryItem("current","1");bad.url.setQuery(params);QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;bad.url.setHost("fapi.binance.com");QVERIFY(!NetworkTradingTransport::allowed(bad));
    }
    void thirtyDayWindowsAndPagesAreBounded(){auto q=query(EarnSource::FlexibleSubscriptions);q.throughMs=q.fromMs+30LL*86400000;QVERIFY(!BinanceEarnProtocol::validQuery(q,q.throughMs));--q.throughMs;QVERIFY(BinanceEarnProtocol::validQuery(q,q.throughMs));q.size=101;QVERIFY(!BinanceEarnProtocol::validQuery(q,q.throughMs));}
    void structuralAndTerminalRecordsDoNotRewindHistory_data(){
        QTest::addColumn<int>("source");QTest::addColumn<QString>("status");QTest::addColumn<QStringList>("issues");
        QTest::newRow("year-old-paid-locked")<<int(EarnSource::LockedRedemptions)<<QString("PAID")<<QStringList{"DESTINATION_UNKNOWN"};
        QTest::newRow("source-split-missing")<<int(EarnSource::FlexibleSubscriptions)<<QString("SUCCESS")<<QStringList{"SOURCE_SPLIT_MISSING"};
        QTest::newRow("source-split-conflict")<<int(EarnSource::FlexibleSubscriptions)<<QString("SUCCESS")<<QStringList{"SOURCE_SPLIT_CONFLICT"};
        QTest::newRow("unknown-reward")<<int(EarnSource::FlexibleRewards)<<QString("REPORTED")<<QStringList{"REWARD_TYPE_UNKNOWN"};
        QTest::newRow("failed")<<int(EarnSource::FlexibleRedemptions)<<QString("FAILED")<<QStringList{"STATUS_UNCONFIRMED"};
        QTest::newRow("pending-but-conflicted")<<int(EarnSource::FlexibleRedemptions)<<QString("CREATED")<<QStringList{"STATUS_UNCONFIRMED","SOURCE_CONFLICT"};
    }
    void structuralAndTerminalRecordsDoNotRewindHistory(){
        QFETCH(int,source);QFETCH(QString,status);QFETCH(QStringList,issues);
        auto s=queriedAccount();const auto r=storedObservation(EarnSource(source),status,Now-365*Day,issues);
        s.observations.append({r.id,1,r,issues.contains("SOURCE_CONFLICT")});const auto original=s;
        const auto jobs=sourceQueries(s,r.source);QCOMPARE(jobs.size(),1);
        QCOMPARE(jobs.first().fromMs,Now-2*Day);QCOMPARE(jobs.first().throughMs,Now);
        QVERIFY(s==original);QVERIFY(!s.observations.first().record.complete);
    }
    void mutablePendingRecordsRevisitOnlyTheirOriginalDay_data(){
        QTest::addColumn<int>("source");QTest::addColumn<QString>("status");QTest::addColumn<qint64>("age");QTest::addColumn<qint64>("delivery");
        for(const auto* status:{"CREATED","PENDING","PROCESSING","WAITING"})
            QTest::newRow(status)<<int(EarnSource::FlexibleRedemptions)<<QString(status)<<3*Day<<qint64(0);
        QTest::newRow("future-locked-payout")<<int(EarnSource::LockedRedemptions)<<QString("PAID")<<365*Day<<Now+Day;
    }
    void mutablePendingRecordsRevisitOnlyTheirOriginalDay(){
        QFETCH(int,source);QFETCH(QString,status);QFETCH(qint64,age);QFETCH(qint64,delivery);
        auto s=queriedAccount();auto r=storedObservation(EarnSource(source),status,Now-age,{"STATUS_UNCONFIRMED"});
        r.deliveryMs=delivery;
        if(delivery){r.issues={"DESTINATION_UNKNOWN","PAYOUT_PENDING"};r.payoutComplete=false;}
        s.observations.append({r.id,1,r,false});const auto jobs=sourceQueries(s,r.source);
        QVERIFY(!jobs.isEmpty());bool coversPending=false,coversRecent=false;
        for(const auto& q:jobs){coversPending|=q.fromMs<=r.timeMs&&q.throughMs>=r.timeMs;
            coversRecent|=q.fromMs<=Now-2*Day&&q.throughMs==Now;QVERIFY(BinanceEarnProtocol::validQuery(q,Now));}
        QVERIFY(coversPending);QVERIFY(coversRecent);
        QVERIFY2(jobs.size()<=2,"A pending original day must not replay every intervening month");
    }
    void pendingRecheckExpiresAtSevenDayBoundary_data(){
        QTest::addColumn<bool>("scheduled");QTest::addColumn<qint64>("offset");QTest::addColumn<bool>("expected");
        for(const bool scheduled:{false,true})for(const qint64 offset:{qint64(-1),qint64(0),qint64(1)})
            QTest::newRow(qPrintable(QString("%1-%2").arg(scheduled?"payout":"request").arg(offset)))<<scheduled<<offset<<(offset<=0);
    }
    void pendingRecheckExpiresAtSevenDayBoundary(){
        QFETCH(bool,scheduled);QFETCH(qint64,offset);QFETCH(bool,expected);auto s=queriedAccount();
        const auto anchor=Now-7*Day-offset;
        auto r=storedObservation(scheduled?EarnSource::LockedRedemptions:EarnSource::FlexibleRedemptions,
            scheduled?"PAID":"CREATED",scheduled?Now-365*Day:anchor,{"STATUS_UNCONFIRMED"});
        if(scheduled){r.deliveryMs=anchor;r.payoutComplete=false;r.issues={"DESTINATION_UNKNOWN","PAYOUT_PENDING"};}
        s.observations.append({r.id,1,r,false});bool rechecks=false;
        for(const auto& q:sourceQueries(s,r.source))rechecks|=q.fromMs<=r.timeMs&&q.throughMs>=r.timeMs;
        QCOMPARE(rechecks,expected);QVERIFY(!s.observations.first().record.complete);
    }
    void redeemingPositionDoesNotReplayPurchaseYear(){
        auto s=queriedAccount();auto r=storedObservation(EarnSource::LockedPositions,"OBSERVED",Now-365*Day,{"REDEMPTION_IN_POSITION"});
        r.redeemingAmount="1";r.deliveryMs=Now+Day;s.positions.append(r);s.positionsAt[int(r.source)]=Now;
        const auto jobs=sourceQueries(s,EarnSource::LockedRedemptions);QCOMPARE(jobs.size(),1);QCOMPARE(jobs.first().fromMs,Now-2*Day);
    }
    void historicalCatchupIsNotCutOffByPendingRetryPolicy(){
        auto s=queriedAccount();for(auto& w:s.windows)w.throughMs=Now-90*Day;
        const auto jobs=sourceQueries(s,EarnSource::FlexibleRedemptions);QVERIFY(jobs.size()>=4);
        QCOMPARE(jobs.first().fromMs,Now-92*Day);QCOMPARE(jobs.last().throughMs,Now);
        for(int i=1;i<jobs.size();++i)QCOMPARE(jobs[i].fromMs,jobs[i-1].throughMs+1);
    }
    void subscriptionPreservesExactIdsAndSplit(){
        auto h=response({subscription()});h.body.replace("\"purchaseId\":\"9007199254740993\"","\"purchaseId\":9007199254740993");
        const auto p=BinanceEarnProtocol::parsePage(query(EarnSource::FlexibleSubscriptions),h,Now);QVERIFY(p.ok);QCOMPARE(p.rows[0].id,QString("9007199254740993"));QCOMPARE(p.rows[0].spotAmount,QString("30"));QCOMPARE(p.rows[0].fundingAmount,QString("70"));QVERIFY(p.rows[0].complete);
        auto row=subscription();row["amtFromSpot"]="31";const auto bad=BinanceEarnProtocol::parsePage(query(EarnSource::FlexibleSubscriptions),response({row}),Now);QVERIFY(bad.ok);QVERIFY(!bad.rows[0].complete);QVERIFY(bad.rows[0].issues.contains("SOURCE_SPLIT_CONFLICT"));
    }
    void positionsNeverAddCumulativeRewards(){const auto p=BinanceEarnProtocol::parsePage(query(EarnSource::FlexiblePositions),response({position()}),Now);QVERIFY(p.ok);QCOMPARE(p.rows[0].amount,QString("103"));QVERIFY(p.rows[0].rewardAmount.isEmpty());QCOMPARE(p.rows[0].timeMs,qint64(0));}
    void lockedRepaymentIsNotInventedSpotDeposit(){
        QJsonObject row{{"positionId","123"},{"redeemId","456"},{"projectId","SYN90"},{"time",QString::number(Now-60000)},
            {"asset","SYN"},{"amount","100"},{"originalAmount","100"},{"lossAmount","2"},{"type","MATURE"},
            {"deliverDate",QString::number(Now-1000)},{"isComplete",true},{"rewardAsset","SYN"},{"rewardAmt","3"},{"estExtraRewardAmt","50"},{"status","PAID"}};
        const auto p=BinanceEarnProtocol::parsePage(query(EarnSource::LockedRedemptions),response({row}),Now);QVERIFY(p.ok);QVERIFY(!p.rows[0].complete);QVERIFY(p.rows[0].destinationWallet.isEmpty());QCOMPARE(p.rows[0].amount,QString("100"));QCOMPARE(p.rows[0].lossAmount,QString("2"));QCOMPARE(p.rows[0].rewardAmount,QString("3"));
    }
    void paidFlexibleRedemptionTracksDestination(){QJsonObject row{{"amount","100"},{"asset","SYN"},{"time",QString::number(Now-1000)},{"projectId","SYN001"},{"redeemId","1"},{"destAccount","SPOT"},{"status","PAID"}};auto p=BinanceEarnProtocol::parsePage(query(EarnSource::FlexibleRedemptions),response({row}),Now);QVERIFY(p.ok);QVERIFY(p.rows[0].complete);row["status"]="PROCESSING";p=BinanceEarnProtocol::parsePage(query(EarnSource::FlexibleRedemptions),response({row}),Now);QVERIFY(p.ok);QVERIFY(!p.rows[0].complete);}
    void flexibleRedemptionWalletAliases_data(){
        QTest::addColumn<QString>("raw");QTest::addColumn<QString>("canonical");QTest::addColumn<bool>("complete");
        QTest::newRow("spot")<<QString("SPOT")<<QString("SPOT")<<true;
        QTest::newRow("fund-alias")<<QString("FUND")<<QString("FUNDING")<<true;
        QTest::newRow("funding-history")<<QString("FUNDING")<<QString("FUNDING")<<true;
        QTest::newRow("unknown")<<QString("UNRECOGNIZED")<<QString("UNRECOGNIZED")<<false;
    }
    void flexibleRedemptionWalletAliases(){
        QFETCH(QString,raw);QFETCH(QString,canonical);QFETCH(bool,complete);
        const QJsonObject row{{"amount","100"},{"asset","SYN"},{"time",QString::number(Now-1000)},
            {"projectId","SYN001"},{"redeemId","1"},{"destAccount",raw},{"status","PAID"}};
        const auto p=BinanceEarnProtocol::parsePage(query(EarnSource::FlexibleRedemptions),response({row}),Now);
        QVERIFY(p.ok);QCOMPARE(p.rows.first().destinationWallet,canonical);QCOMPARE(p.rows.first().complete,complete);
        QCOMPARE(p.rows.first().issues.contains("DESTINATION_UNKNOWN"),!complete);
    }
    void legacyFundingSpellingDoesNotCreateSourceConflict(){
        auto s=account();s.revision=1;auto r=storedObservation(EarnSource::FlexibleRedemptions,"PAID",Now-1000);
        r.destinationWallet="FUND";r.complete=true;s.observations.append({r.id,1,r,false});
        EarnAccountState decoded;QVERIFY(EarnTracking::decode(EarnTracking::encode(s),&decoded));
        QCOMPARE(decoded.observations.first().record.destinationWallet,QString("FUNDING"));
        auto other=r;other.destinationWallet="FUNDING";QVERIFY(EarnTracking::sameMeaning(r,other));
        r.destinationWallet="FUNDING";r.complete=false;r.issues={"DESTINATION_UNKNOWN"};
        s.observations[0].record=r;QVERIFY(EarnTracking::decode(EarnTracking::encode(s),&decoded));
        QVERIFY(decoded.observations.first().record.complete);QVERIFY(decoded.observations.first().record.issues.isEmpty());
        r.issues<<"SOURCE_CONFLICT";s.observations[0]={r.id,1,r,true};
        QVERIFY(EarnTracking::decode(EarnTracking::encode(s),&decoded));
        QVERIFY(decoded.observations.first().conflict);QVERIFY(!decoded.observations.first().record.complete);
    }
    void duplicateRewardsRemainAmbiguous(){QJsonObject row{{"asset","SYN"},{"rewards","1"},{"projectId","SYN001"},{"type","BONUS"},{"time",QString::number(Now-1000)}};const auto p=BinanceEarnProtocol::parsePage(query(EarnSource::FlexibleRewards),response({row,row}),Now);QVERIFY(!p.ok);QCOMPARE(p.errorCode,QString("REWARD_ID_AMBIGUOUS"));}
    void duplicateJsonMembersAndEncodedAliasesAreRejected(){
        auto h=response({subscription()});h.body.replace("\"amount\":\"100\"","\"amount\":\"100\",\"amou\\u006et\":\"1\"");
        const auto p=BinanceEarnProtocol::parsePage(query(EarnSource::FlexibleSubscriptions),h,Now);QVERIFY(!p.ok);QCOMPARE(p.errorCode,QString("INVALID_JSON"));
    }
    void lockedSourceContractsAndRewardTypesRemainDistinct(){
        QJsonObject pos{{"positionId","123"},{"projectId","Syn*90"},{"asset","SYN"},{"amount","100"},
            {"purchaseTime",QString::number(Now-60000)},{"redeemingAmt","0"},{"autoSubscribe",true},
            {"redeemTo","FLEXIBLE"},{"deliverDate",QString::number(Now+86400000)},{"rewardAmt","99"},{"estTotalRewardAmt","1000"}};
        auto p=BinanceEarnProtocol::parsePage(query(EarnSource::LockedPositions),response({pos}),Now);QVERIFY(p.ok);QCOMPARE(p.rows[0].destinationWallet,QString("FLEXIBLE"));QVERIFY(p.rows[0].rewardAmount.isEmpty());QCOMPARE(p.rows[0].amount,QString("100"));
        auto sub=subscription();sub.remove("productId");sub["projectId"]="Syn*90";sub["positionId"]="123";
        p=BinanceEarnProtocol::parsePage(query(EarnSource::LockedSubscriptions),response({sub}),Now);QVERIFY(p.ok);QVERIFY(p.rows[0].complete);QCOMPARE(p.rows[0].positionId,QString("123"));
        QJsonObject reward{{"positionId","123"},{"asset","SYN"},{"amount","0.1"},{"type","Locked Rewards"},{"time",QString::number(Now-1000)}};
        p=BinanceEarnProtocol::parsePage(query(EarnSource::LockedRewards),response({reward}),Now);QVERIFY(p.ok);QVERIFY(p.rows[0].complete);QCOMPARE(p.rows[0].amount,QString("0.1"));
        reward["type"]="Future Reward";p=BinanceEarnProtocol::parsePage(query(EarnSource::LockedRewards),response({reward}),Now);QVERIFY(p.ok);QVERIFY(!p.rows[0].complete);QVERIFY(p.rows[0].issues.contains("REWARD_TYPE_UNKNOWN"));
        SecureBuffer key(32);randombytes_buf(key.data(),key.size());EarnAccountState next;QString error;
        QVERIFY2(EarnTracking::merge(account(),parsed(EarnSource::LockedPositions,{pos}),key,&next,&error),qPrintable(error));
        EarnAccountState decoded;QVERIFY(EarnTracking::decode(EarnTracking::encode(next),&decoded));QVERIFY(decoded==next);
    }
    void paginationRequiresAllRowsAndStableTotal(){
        EarnPageAccumulator a;auto q=query(EarnSource::FlexibleSubscriptions,1,1);auto p=BinanceEarnProtocol::parsePage(q,response({subscription("1")},2),Now);QString error;QVERIFY(a.add(q,p,Now,&error));QVERIFY(!a.complete());++q.page;p=BinanceEarnProtocol::parsePage(q,response({subscription("2")},2),Now);QVERIFY(a.add(q,p,Now,&error));QVERIFY(a.complete());QCOMPARE(a.batch().rows.size(),2);
        a.clear();q.page=1;p=BinanceEarnProtocol::parsePage(q,response({subscription("1")},2),Now);QVERIFY(a.add(q,p,Now));q.page=2;p.total=3;QVERIFY(!a.add(q,p,Now));QVERIFY(!a.complete());
    }
    void sourceStatePersistsWithoutRawIdsAndReceiptConflicts(){
        SecureBuffer key(32);randombytes_buf(key.data(),key.size());auto s=account();EarnAccountState next;QString error;
        auto b=parsed(EarnSource::FlexibleSubscriptions,{subscription()});QVERIFY2(EarnTracking::merge(s,b,key,&next,&error),qPrintable(error));
        QCOMPARE(next.observations.size(),1);QVERIFY(next.observations[0].record.id!=QString("9007199254740993"));
        const auto encoded=EarnTracking::encode(next);QVERIFY(!QJsonDocument(encoded).toJson().contains("9007199254740993"));EarnAccountState decoded;QVERIFY(EarnTracking::decode(encoded,&decoded));QVERIFY(decoded==next);
        s=next;b.receivedAtMs+=1000;for(auto& row:b.rows)row.receivedAtMs+=1000;
        QVERIFY2(EarnTracking::merge(s,b,key,&next,&error),qPrintable(error));QCOMPARE(next.observations.size(),1);
    }
    void pendingCompletionIsOneActiveObservationAndConflictsStayVisible(){
        SecureBuffer key(32);randombytes_buf(key.data(),key.size());auto s=account();EarnAccountState next;QString error;
        auto b=parsed(EarnSource::FlexibleSubscriptions,{subscription("1","PROCESSING")});QVERIFY(EarnTracking::merge(s,b,key,&next,&error));s=next;
        b=parsed(EarnSource::FlexibleSubscriptions,{subscription("1","SUCCESS")});QVERIFY2(EarnTracking::merge(s,b,key,&next,&error),qPrintable(error));QCOMPARE(next.observations.size(),2);QCOMPARE(EarnTracking::active(next).size(),1);QVERIFY(EarnTracking::active(next).first().record.complete);
        s=next;b.rows[0].amount="101";QVERIFY(EarnTracking::merge(s,b,key,&next,&error));QVERIFY(EarnTracking::active(next).first().conflict);QVERIFY(!EarnTracking::active(next).first().record.complete);
    }
    void unknownFieldsCannotEnterPrivateStore(){auto encoded=EarnTracking::encode(account());encoded["address"]="synthetic-private-address";EarnAccountState out;QVERIFY(!EarnTracking::decode(encoded,&out));}
    void authenticationAndDiagnosticsDoNotEchoPrivateData(){
        HttpResult h;h.status=401;h.body="synthetic-sensitive-account";auto p=BinanceEarnProtocol::parsePage(query(EarnSource::FlexibleSubscriptions),h,Now);QVERIFY(!p.ok);QVERIFY(!p.errorCode.contains("synthetic"));
        ResponseJournal::setEnabled(true);ResponseJournal::clear();ResponseJournal::recordTrading(Operation::EarnRead,"GET",QUrl("https://api.binance.com/sapi/v1/simple-earn/flexible/position"),true,h);QVERIFY(ResponseJournal::entries().isEmpty());ResponseJournal::setEnabled(false);
    }
    void schemaThreeMigrationBacksUpExactOldCiphertextAndReopens(){
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));AssetsLedger ledger(&v,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));
        QFile before(ledger.filePath());QVERIFY(before.open(QIODevice::ReadOnly));const auto original=before.readAll();before.close();
        auto state=account();state.connectionId=v.accounts().first().id;state.binding=snapshot(v).connectionBinding;EarnAccountState next;QString error;
        QVERIFY(v.withAssetsLedgerKey(false,[&](const SecureBuffer& key){QVERIFY2(EarnTracking::merge(state,parsed(EarnSource::FlexibleSubscriptions,{subscription()}),key,&next,&error),qPrintable(error));}));
        QSignalSpy saved(&ledger,&AssetsLedger::collectionWriteFinished);QCOMPARE(ledger.saveEarnState(next,0,71,&error),AssetsLedger::CollectionWrite::Pending);QTRY_COMPARE(saved.size(),1);QCOMPARE(saved[0][0].toULongLong(),quint64(71));QVERIFY(saved[0][1].toBool());
        const auto backups=QDir(dir.path()).entryList({"assets.keefi.pre-schema3-*.keefi"},QDir::Files);QCOMPARE(backups.size(),1);QFile backup(dir.filePath(backups.first()));QVERIFY(backup.open(QIODevice::ReadOnly));QCOMPARE(backup.readAll(),original);backup.close();
        QVERIFY(Vault::erasedFileNames(v.filePath()).contains(backups.first()));QVERIFY(!ledger.archiveFiles().contains(backups.first()));
        ledger.lock();QVERIFY(ledger.open(false));QCOMPARE(ledger.earnStates().size(),1);QVERIFY(ledger.earnStates().first()==next);
        QFile cipher(ledger.filePath());QVERIFY(cipher.open(QIODevice::ReadOnly));const auto raw=cipher.readAll();QVERIFY(!raw.contains("SYN001"));QVERIFY(!raw.contains("9007199254740993"));
        saved.clear();QCOMPARE(ledger.saveEarnState(next,next.revision,72,&error),AssetsLedger::CollectionWrite::Unchanged);QCOMPARE(saved.size(),0);
        QCOMPARE(ledger.saveEarnState(next,0,73,&error),AssetsLedger::CollectionWrite::Failed);QVERIFY(ledger.earnStates().first()==next);
        AssetsLedger old(&v,dir.filePath(backups.first()));QVERIFY(old.open(false));QVERIFY(old.earnStates().isEmpty());
    }
    void historyRolloverCarriesCollectionState(){
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));AssetsLedger ledger(&v,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));auto s=snapshot(v);
        EarnAccountState start=account(),next;start.connectionId=s.connectionId;start.binding=s.connectionBinding;QString error;
        QVERIFY(v.withAssetsLedgerKey(false,[&](const SecureBuffer& key){QVERIFY(EarnTracking::merge(start,parsed(EarnSource::FlexibleSubscriptions,{subscription()}),key,&next,&error));}));
        QSignalSpy saved(&ledger,&AssetsLedger::collectionWriteFinished);QCOMPARE(ledger.saveEarnState(next,0,81,&error),AssetsLedger::CollectionWrite::Pending);QTRY_COMPARE(saved.size(),1);QVERIFY(saved[0][1].toBool());
        QList<AssetSnapshot> rows;for(int i=0;i<AssetsLedger::MaxSnapshots;++i){auto row=s;row.id=QString::number(i);row.timeMs-=i;row.evidence.receivedAtMs=row.timeMs;rows.append(row);}QVERIFY2(ledger.appendWithArchive(rows,{},{},{},&error),qPrintable(error));
        s.id="rollover";QVERIFY2(ledger.appendWithArchive({s},{},{},{},&error),qPrintable(error));QCOMPARE(ledger.archiveFiles().size(),1);QVERIFY(ledger.earnStates().first()==next);ledger.lock();QVERIFY(ledger.open(false));QVERIFY(ledger.earnStates().first()==next);
    }
    void collectorTracksEightReadSourcesAndResumesFromDurableState(){
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));AssetsLedger ledger(&v,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));EarnWire wire;
        BinanceEarnCollector::TestHooks hooks;hooks.nowMs=[] {return Now;};hooks.pacingMs=0;
        BinanceEarnCollector collector(v,ledger,&wire,hooks);QSignalSpy finished(&collector,&BinanceEarnCollector::finished);
        QVERIFY(collector.start(snapshot(v)));QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,15000);QVERIFY2(finished[0][0].toBool(),qPrintable(collector.errorCode()));QVERIFY(!collector.busy());
        QSet<QString> paths;for(const auto& request:wire.requests){QCOMPARE(request.method,QByteArray("GET"));QVERIFY(NetworkTradingTransport::allowed(request));if(request.operation==Operation::EarnRead)paths.insert(request.url.path());}QCOMPARE(paths.size(),8);
        auto saved=ledger.earnStates();QCOMPARE(saved.size(),1);QCOMPARE(saved[0].positions.size(),1);QCOMPARE(EarnTracking::active(saved[0]).size(),1);QCOMPARE(saved[0].windows.size(),6);
        const auto count=saved[0].observations.size();finished.clear();QVERIFY(collector.start(snapshot(v)));QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,15000);QVERIFY(finished[0][0].toBool());QCOMPARE(ledger.earnStates()[0].observations.size(),count);
    }
    void lockBeforeServerTimePreventsSigningAndLateResults(){
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));AssetsLedger ledger(&v,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));EarnWire wire;wire.delayed=true;
        BinanceEarnCollector::TestHooks hooks;hooks.nowMs=[] {return Now;};hooks.pacingMs=0;BinanceEarnCollector collector(v,ledger,&wire,hooks);
        QVERIFY(collector.start(snapshot(v)));QTRY_COMPARE(wire.requests.size(),1);QCOMPARE(wire.requests[0].operation,Operation::Time);v.lock();wire.deliver();QVERIFY(!collector.busy());QCOMPARE(wire.requests.size(),1);QVERIFY(collector.status().isEmpty());
    }
    void collectorUsesServerBoundedHistoryWindow(){
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));AssetsLedger ledger(&v,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));
        EarnWire wire;wire.serverNow=Now-5000;BinanceEarnCollector::TestHooks hooks;hooks.nowMs=[] {return Now;};hooks.pacingMs=0;
        BinanceEarnCollector collector(v,ledger,&wire,hooks);QSignalSpy finished(&collector,&BinanceEarnCollector::finished);
        QVERIFY(collector.start(snapshot(v)));QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,15000);
        QVERIFY2(finished[0][0].toBool(),qPrintable(collector.errorCode()));QVERIFY(collector.errorCode().isEmpty());
        int histories=0;for(const auto& r:wire.requests)if(r.operation==Operation::EarnRead){
            const QUrlQuery params(r.url);if(params.hasQueryItem("endTime")){
                ++histories;QCOMPARE(params.queryItemValue("endTime").toLongLong(),wire.serverNow);
                QVERIFY(params.queryItemValue("endTime").toLongLong()<=params.queryItemValue("timestamp").toLongLong());
            }
        }
        QCOMPARE(histories,6);const auto saved=ledger.earnStates();QCOMPARE(saved.size(),1);QCOMPARE(saved[0].windows.size(),6);
        for(const auto& w:saved[0].windows)QCOMPARE(w.throughMs,wire.serverNow);
        QCOMPARE(saved[0].trackingStartMs,wire.serverNow);
    }
    void clockSkewHasSpecificGuidanceWithoutBindingFailure(){
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(prepare(v));AssetsLedger ledger(&v,dir.filePath("assets.keefi"));QVERIFY(ledger.open(true));
        EarnWire wire;wire.serverNow=Now-120001;BinanceEarnCollector::TestHooks hooks;hooks.nowMs=[] {return Now;};hooks.pacingMs=0;
        BinanceEarnCollector collector(v,ledger,&wire,hooks);QSignalSpy finished(&collector,&BinanceEarnCollector::finished);
        QVERIFY(collector.start(snapshot(v)));QTRY_COMPARE(finished.size(),1);QVERIFY(!finished[0][0].toBool());
        QCOMPARE(collector.errorCode(),QString("CLOCK_SKEW"));QVERIFY(collector.status().contains("시간"));
        QVERIFY(!collector.status().contains("계좌 연결"));QVERIFY(ledger.earnStates().isEmpty());QCOMPARE(wire.requests.size(),1);
    }
};
QTEST_GUILESS_MAIN(BinanceEarnTest)
#include "tst_binance_earn.moc"
