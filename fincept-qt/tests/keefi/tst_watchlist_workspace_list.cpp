#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/TradingTypes.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <ctime>
#include <QBuffer>
#include <cstring>

using namespace keefi;
namespace {
class CountedCsvInput final : public QIODevice {
public:
    explicit CountedCsvInput(qint64 total,qint64 chunk=0):total_(total),chunk_(chunk){open(QIODevice::ReadOnly|QIODevice::Unbuffered);}
    explicit CountedCsvInput(const QByteArray& bytes,qint64 chunk=0):CountedCsvInput(qint64(bytes.size()),chunk){payload_=bytes;}
    bool isSequential() const override{return true;}
    qint64 bytesAvailable() const override{return total_-consumed_+QIODevice::bytesAvailable();}
    qint64 consumed() const{return consumed_;}
    qint64 largestRequested() const{return largestRequested_;}
protected:
    qint64 readData(char* data,qint64 maximum) override{
        largestRequested_=qMax(largestRequested_,maximum);
        qint64 count=qMin(maximum,total_-consumed_);
        if(chunk_>0)count=qMin(count,chunk_);
        if(count>0){
            if(payload_.isEmpty())std::memset(data,'X',size_t(count));
            else std::memcpy(data,payload_.constData()+consumed_,size_t(count));
        }
        consumed_+=count;return count;
    }
    qint64 writeData(const char*,qint64) override{return -1;}
private:
    qint64 total_=0,chunk_=0,consumed_=0,largestRequested_=0;
    QByteArray payload_;
};
}
class WatchlistWorkspaceListTest final : public QObject {
    Q_OBJECT
  private slots:
    void csvDeviceBudgetHandlesLimitsAndShortReads_data(){
        QTest::addColumn<qint64>("size");QTest::addColumn<qint64>("chunk");
        constexpr qint64 limit=1024*1024;
        QTest::newRow("empty")<<qint64(0)<<qint64(0);
        QTest::newRow("below-limit")<<(limit-1)<<qint64(0);
        QTest::newRow("at-limit")<<limit<<qint64(0);
        QTest::newRow("one-over")<<(limit+1)<<qint64(0);
        QTest::newRow("many-over")<<(limit+8192)<<qint64(0);
        QTest::newRow("short-reads")<<(limit+8192)<<qint64(4093);
    }
    void csvDeviceBudgetHandlesLimitsAndShortReads(){
        QFETCH(qint64,size);QFETCH(qint64,chunk);constexpr qint64 limit=1024*1024;
        CountedCsvInput input(size,chunk);WatchlistWorkspaceList lists;
        const auto before=lists.publicState();QSignalSpy changes(&lists,&WatchlistWorkspaceList::changed);QString error;
        QVERIFY(lists.importCsv(input,&error).isEmpty());
        QCOMPARE(input.consumed(),qMin(size,limit+1));
        QVERIFY(input.largestRequested()<=64*1024);
        QVERIFY(!error.isEmpty());QCOMPARE(lists.publicState(),before);QCOMPARE(changes.size(),0);
    }
    void csvDeviceSmallValidInputUsesFixedReadChunks(){
        WatchlistWorkspaceList original;const auto id=original.createList("합성 작은 CSV","blue");
        QVERIFY(original.addIdentity(id,{"coin","upbit","KRW-BTC"}));
        const auto bytes=original.exportCsv(id);QVERIFY(!bytes.isEmpty());QVERIFY(bytes.size()<64*1024);
        CountedCsvInput input(bytes,17);QVERIFY(input.isReadable());
        WatchlistWorkspaceList restored;QString error;QSignalSpy changes(&restored,&WatchlistWorkspaceList::changed);
        const auto imported=restored.importCsv(input,&error);QVERIFY(!imported.isEmpty());QVERIFY(error.isEmpty());
        QCOMPARE(restored.exportCsv(imported),bytes);QCOMPARE(changes.size(),1);QVERIFY(input.atEnd());
        QCOMPARE(input.consumed(),qint64(bytes.size()));
        QVERIFY(input.largestRequested()>0);QVERIFY(input.largestRequested()<=64*1024);
    }
    void csvDevicePreservesValidRoundTrip(){
        WatchlistWorkspaceList original;const auto id=original.createList("합성 A&B <리스트>","blue");
        const WatchlistIdentity identity{"coin","upbit","KRW-BTC"};QVERIFY(original.addIdentity(id,identity));
        auto bytes=original.exportCsv(id);QBuffer input(&bytes);QVERIFY(input.open(QIODevice::ReadOnly));
        WatchlistWorkspaceList restored;QString error;QSignalSpy changes(&restored,&WatchlistWorkspaceList::changed);
        const auto imported=restored.importCsv(input,&error);QVERIFY(!imported.isEmpty());QVERIFY(error.isEmpty());
        QCOMPARE(restored.userList(imported).name,original.userList(id).name);
        QCOMPARE(restored.userList(imported).flag,original.userList(id).flag);
        QCOMPARE(restored.userList(imported).entries,original.userList(id).entries);
        QCOMPARE(changes.size(),1);QVERIFY(input.atEnd());
    }
    void csvDeviceRejectsOversizeBeforeReadingBeyondBudget(){
        constexpr qint64 limit=1024*1024;
        CountedCsvInput input(2*limit);
        WatchlistWorkspaceList lists;lists.createList("합성 보존 목록");
        const auto before=lists.publicState();QSignalSpy changes(&lists,&WatchlistWorkspaceList::changed);
        QString error;
        QVERIFY(lists.importCsv(input,&error).isEmpty());
        QVERIFY2(input.consumed()<=limit+1,"CSV file import must not consume beyond the existing 1 MiB limit plus one sentinel byte");
        QVERIFY(!error.isEmpty());QCOMPARE(lists.publicState(),before);QCOMPARE(changes.size(),0);
    }
    void visibleAdmittedBinanceQuoteSurvivesMetadataRefreshWithoutPromotingUnsupportedMarkets(){
        const WatchlistIdentity identity{"coin","binance","SYNUSDT"};constexpr qint64 received=1000000;
        PublicTick tick;tick.market=identity.code;tick.signedChangeRate="0.2345";tick.streamEpoch=1;
        tick.quote={"SYN","USDT","1.2345",{"Binance 공식 USDT 현재가","wss://stream.binance.com:9443/ws",
            "public:binance:SYNUSDT",received,received,true}};
        const auto original=tick.quote.evidence;
        MarketInstrument supported;supported.symbol="SYNUSDT";supported.baseAsset="SYN";supported.quoteAsset="USDT";
        supported.market="spot";supported.status="TRADING";supported.tradable=true;
        QCOMPARE(projectRetainedBinanceWatchlistQuote(identity,tick,{supported},received,false,1).quoteState,WatchlistQuoteState::Receiving);
        const auto waiting=projectRetainedBinanceWatchlistQuote(identity,tick,{},received+1,false,1);
        QCOMPARE(waiting.price,QString("1.2345"));QCOMPARE(waiting.change,QString("+23.45%"));
        QCOMPARE(waiting.quoteState,WatchlistQuoteState::Delayed);QCOMPARE(waiting.quoteCurrency,QString("USDT"));
        QCOMPARE(tick.quote.evidence.sourceTimeMs,original.sourceTimeMs);QCOMPARE(tick.quote.evidence.receivedAtMs,original.receivedAtMs);
        auto removed=supported;removed.symbol="ALTUSDT";removed.baseAsset="ALT";
        auto halted=supported;halted.status="BREAK";halted.tradable=false;
        auto nonSpot=supported;nonSpot.market="future";
        for(const auto& catalogue:{QList<MarketInstrument>{removed},QList<MarketInstrument>{halted},QList<MarketInstrument>{nonSpot},QList<MarketInstrument>{supported,supported}}){
            const auto rejected=projectRetainedBinanceWatchlistQuote(identity,tick,catalogue,received+1);
            QVERIFY(rejected.price.isEmpty());QCOMPARE(rejected.quoteState,WatchlistQuoteState::Unsupported);
        }
        QCOMPARE(projectRetainedBinanceWatchlistQuote(identity,tick,{supported},received+90001,false,1).quoteState,WatchlistQuoteState::Delayed);
        QCOMPARE(projectRetainedBinanceWatchlistQuote(identity,tick,{supported},received+1,false,2).quoteState,WatchlistQuoteState::Delayed);
        QCOMPARE(projectRetainedBinanceWatchlistQuote(identity,tick,{},received+1,true,1).quoteState,WatchlistQuoteState::Disconnected);
        auto wrong=tick;wrong.quote.evidence.sourceTimeMs=received-90001;
        QVERIFY(projectRetainedBinanceWatchlistQuote(identity,wrong,{},received+1).price.isEmpty());
    }
    void publicStockCatalogueAllowsSavingWithoutBrokerAuthorization() {
        WatchlistWorkspaceList lists;
        const WatchlistIdentity kr{"stock", "KR", "005930"}, us{"stock", "US", "NVDA"};
        const auto id = lists.createList("공개 대표 종목");
        const auto initial = lists.publicState();
        lists.setPublicStockInstruments({kr, us, kr, {"stock", "US", "invalid"}});
        QCOMPARE(lists.publicState(), initial); // Registry is transient, not a saved permission.
        QVERIFY(!lists.isCurrentlyVerified(kr)); QVERIFY(!lists.isCurrentlyVerified(us));
        QVERIFY(lists.addIdentity(id, kr));
        lists.clearPrivate();
        QVERIFY(lists.addIdentity(id, us)); // Public catalogue survives vault locking.
        lists.setHoldings({kr, us}); QVERIFY(lists.holdings().isEmpty());
        QVERIFY(!lists.isCurrentlyVerified(kr)); QVERIFY(!lists.isCurrentlyVerified(us));
        QCOMPARE(lists.userList(id).entries.size(), 2);
    }
    void publicStockPermissionCanBeRevokedWithoutChangingSavedOrder() {
        WatchlistWorkspaceList lists;
        const WatchlistIdentity kr{"stock", "KR", "005930"}, us{"stock", "US", "NVDA"};
        const auto id = lists.createList("대표 목록"), another = lists.createList("다른 목록");
        const auto korea = lists.addSection(id, "한국"), america = lists.addSection(id, "미국");
        lists.setPublicStockInstruments({kr, us});
        QVERIFY(lists.addIdentity(id, kr, korea)); QVERIFY(lists.addIdentity(id, us, america));
        QVERIFY(lists.moveSection(id, america, 0)); QVERIFY(lists.moveIdentity(id, us, 0, america));
        QVERIFY(lists.moveList(id, 1));
        const auto saved = lists.publicState();
        lists.setPublicStockInstruments({});
        QVERIFY(!lists.addIdentity(another, kr)); QVERIFY(!lists.addIdentity(another, us));
        QCOMPARE(lists.publicState(), saved);
        WatchlistWorkspaceList restored;
        QVERIFY(restored.restorePublicState(saved));
        QCOMPARE(restored.userLists(), lists.userLists());
        QVERIFY(!restored.isCurrentlyVerified(kr));
        QVERIFY(!restored.addIdentity(another, kr)); // Restoring names cannot restore permission.
        QCOMPARE(restored.userList(id).sections.first().id, america);
        QCOMPARE(restored.userList(id).entries.first().identity, us);
    }
    void repairOneChosenIdentityPreservesPlacementAndAvoidsDuplicates() {
        WatchlistWorkspaceList lists;
        const WatchlistIdentity old{"coin", "upbit", "KRW-HYPE"}, target{"coin", "binance", "HYPEUSDT"};
        const WatchlistIdentity btc{"coin", "upbit", "KRW-BTC"}, stock{"stock", "US", "NVDA"};
        const auto id = lists.createList("수정할 목록"), other = lists.createList("보존할 목록");
        const auto section = lists.addSection(id, "코인"), second = lists.addSection(id, "별도");
        QVERIFY(lists.addIdentity(id, btc, section)); QVERIFY(lists.addIdentity(id, old, section));
        QVERIFY(lists.addIdentity(other, old));
        const auto untouched = lists.userList(other);
        QSignalSpy changed(&lists, &WatchlistWorkspaceList::changed);
        QVERIFY(lists.replaceIdentity(id, old, target));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(lists.userList(id).entries[1].identity, target);
        QCOMPARE(lists.userList(id).entries[1].sectionId, section);
        QCOMPARE(lists.userList(other), untouched);
        QVERIFY(!lists.replaceIdentity(id, target, stock)); // Public stock needs catalogue authorization.
        QCOMPARE(changed.size(), 1);
        QVERIFY(lists.addIdentity(id, old, second));
        changed.clear();
        QVERIFY(lists.replaceIdentity(id, old, target));
        QCOMPARE(changed.size(), 1); QCOMPARE(lists.userList(id).entries.size(), 2);
        QCOMPARE(lists.userList(id).entries[1].identity, target);
        QCOMPARE(lists.userList(id).entries[1].sectionId, section); // Existing target wins.
        QVERIFY(lists.replaceIdentity(id, target, target)); QCOMPARE(changed.size(), 1);
        QVERIFY(!lists.replaceIdentity(id, old, target)); QCOMPARE(changed.size(), 1);
        lists.setPublicStockInstruments({stock});
        QVERIFY(lists.replaceIdentity(id, target, stock));
        QVERIFY(!lists.isCurrentlyVerified(stock));
        WatchlistWorkspaceList restored;
        QVERIFY(restored.restorePublicState(lists.publicState()));
        QCOMPARE(restored.userLists(), lists.userLists());
    }
    void publicQuoteRetentionRequiresExactIdentityCurrencyAndOriginalFreshness() {
        const WatchlistIdentity identity{"coin","binance","SYNUSDT"};constexpr qint64 received=1000000;
        PublicTick tick;tick.market=identity.code;tick.signedChangeRate="0.2345";
        tick.quote={"SYN","USDT","1.2345",{"Binance 공식 USDT 현재가","wss://stream.binance.com:9443/ws",
            "public:binance:SYNUSDT",received,received,true}};
        QCOMPARE(projectBinanceWatchlistQuote(identity,tick,received).price,QString("1.2345"));
        QCOMPARE(projectBinanceWatchlistQuote(identity,tick,received+90000).quoteState,WatchlistQuoteState::Receiving);
        QCOMPARE(projectBinanceWatchlistQuote(identity,tick,received).quoteCurrency,QString("USDT"));
        tick.streamEpoch=1;
        QCOMPARE(projectBinanceWatchlistQuote(identity,tick,received,false,2).quoteState,WatchlistQuoteState::Delayed);
        QCOMPARE(projectBinanceWatchlistQuote(identity,tick,received,false,1).quoteState,WatchlistQuoteState::Receiving);
        const auto delayed=projectBinanceWatchlistQuote(identity,tick,received+90001);
        QCOMPARE(delayed.price,QString("1.2345"));QCOMPARE(delayed.change,QString("+23.45%"));
        QCOMPARE(delayed.quoteState,WatchlistQuoteState::Delayed);QVERIFY(!delayed.privateData);
        QCOMPARE(projectBinanceWatchlistQuote(identity,tick,received+90001,true).quoteState,WatchlistQuoteState::Disconnected);
        QVERIFY(projectBinanceWatchlistQuote({"coin","upbit","KRW-SYN"},tick,received).price.isEmpty());
        QVERIFY(projectBinanceWatchlistQuote({"coin","binance","OTHERUSDT"},tick,received).price.isEmpty());
        QVERIFY(projectBinanceWatchlistQuote({"stock","US","SYN"},tick,received).price.isEmpty());
        auto wrong=tick;wrong.market="OTHERUSDT";QVERIFY(projectBinanceWatchlistQuote(identity,wrong,received).price.isEmpty());
        wrong=tick;wrong.quote.baseAsset="OTHER";QVERIFY(projectBinanceWatchlistQuote(identity,wrong,received).price.isEmpty());
        wrong=tick;wrong.quote.quoteAsset="USDC";QVERIFY(projectBinanceWatchlistQuote(identity,wrong,received).price.isEmpty());
        wrong=tick;wrong.quote.evidence.scope="private:binance:SYNUSDT";QVERIFY(projectBinanceWatchlistQuote(identity,wrong,received).price.isEmpty());
        wrong=tick;wrong.quote.evidence.complete=false;QVERIFY(projectBinanceWatchlistQuote(identity,wrong,received).price.isEmpty());
        wrong=tick;wrong.quote.evidence.sourceTimeMs=received-90001;
        QVERIFY2(projectBinanceWatchlistQuote(identity,wrong,received).price.isEmpty(),"Already stale on first receipt must not become a last-known price");
        wrong=tick;wrong.quote.evidence.sourceTimeMs=received+2001;QVERIFY(projectBinanceWatchlistQuote(identity,wrong,received+5000).price.isEmpty());
        QVERIFY(projectBinanceWatchlistQuote(identity,tick,received-1).price.isEmpty());
        wrong=tick;wrong.quote.price="-1";QVERIFY(projectBinanceWatchlistQuote(identity,wrong,received).price.isEmpty());
        wrong=tick;wrong.quote.price="1e9";QVERIFY(projectBinanceWatchlistQuote(identity,wrong,received).price.isEmpty());
        WatchlistWorkspaceList lists;const auto id=lists.createList("합성 공개 관측");QVERIFY(lists.addIdentity(id,identity));
        const auto saved=lists.publicState();lists.setRowState(identity,delayed);QCOMPARE(lists.rowState(identity).price,QString("1.2345"));
        QCOMPARE(lists.publicState(),saved);QVERIFY(!lists.publicState().contains("1.2345"));
    }
    void repeatedIdentityValidationKeepsExactGrammarAndCpuBudget() {
        const QList<QPair<WatchlistIdentity, bool>> cases{
            {{"coin", "upbit", "KRW-BTC"}, true},
            {{"coin", "bithumb", "BTC-" + QString(20, 'A')}, true},
            {{"coin", "upbit", "KRW-" + QString(21, 'A')}, false},
            {{"coin", "upbit", "KRW-BTC\n"}, false},
            {{"coin", "upbit", "KRW-ＢＴＣ"}, false},
            {{"stock", "KR", "0101N0"}, true},
            {{"stock", "KR", "0101n0"}, false},
            {{"stock", "US", "BRK.B"}, true},
            {{"stock", "US", "ABCDEFGH-A"}, true},
            {{"stock", "US", "ABCDEFGHI"}, false},
            {{"stock", "US", "A.B.C"}, false},
            {{"stock", "US", "A\n"}, false},
        };
        WatchlistWorkspaceList lists;
        const WatchlistIdentity unverified{"stock", "US", "NVDA"};
        const auto saved = lists.publicState();
        const auto start = std::clock();
        // Repeated unchanged catalogue validation must not monopolize the UI
        // thread. CPU time excludes scheduling contention in the full suite.
        for (int round = 0; round < 2000; ++round) for (const auto& item : cases) {
            QCOMPARE(WatchlistWorkspaceList::validIdentity(item.first), item.second);
            QCOMPARE(WatchlistWorkspaceList::identityFromKey(item.first.key()) == item.first, item.second);
        }
        const double cpuMs = 1000.0 * (std::clock() - start) / CLOCKS_PER_SEC;
        qInfo() << "Repeated exact identity validation CPU ms:" << cpuMs;
        QVERIFY2(cpuMs < 250.0, qPrintable(QString("Repeated public identity validation used %1 ms CPU").arg(cpuMs)));
        QVERIFY(!lists.isCurrentlyVerified(unverified));
        QCOMPARE(lists.publicState(), saved);
    }
    void userListsKeepMixedPublicIdentitiesAndOrder() {
        WatchlistWorkspaceList lists;
        const auto first = lists.createList("합성 혼합 리스트", "red");
        QVERIFY2(!first.isEmpty(), "Users can create a named public chart list");
        const auto section = lists.addSection(first, "혼합");
        QVERIFY(!section.isEmpty());
        const WatchlistIdentity coin{"coin", "upbit", "KRW-BTC"}, stock{"stock", "US", "NVDA"};
        lists.setVerifiedInstruments({coin, stock});
        QVERIFY(lists.addIdentity(first, coin, section));
        QVERIFY(lists.addIdentity(first, stock, section));
        QCOMPARE(lists.userList(first).entries.size(), 2);
        QCOMPARE(lists.userList(first).entries[0].identity.key(), QString("coin:upbit:KRW-BTC"));
        QCOMPARE(lists.userList(first).entries[1].identity.key(), QString("stock:US:NVDA"));
        WatchlistWorkspaceList restored;
        QVERIFY(restored.restorePublicState(lists.publicState()));
        QCOMPARE(restored.userLists(), lists.userLists());
    }
    void privateObservationsNeverEnterPublicStateAndLockPreservesUserLists() {
        WatchlistWorkspaceList lists;
        const WatchlistIdentity user{"coin", "upbit", "KRW-BTC"}, held{"coin", "binance", "ETHUSDT"};
        const auto id = lists.createList("합성 공개 리스트", "green");
        QVERIFY(lists.addIdentity(id, user));
        const auto publicOnly = lists.publicState();
        lists.setHoldings({held, held});
        QCOMPARE(lists.holdings().size(), 1);
        const WatchlistRowState privateRow{"FAKE_PRIVATE_PRICE", "+FAKE_PRIVATE_CHANGE", "FAKE_PRIVATE_ALIAS", {"FAKE_PRIVATE_BADGE"}, 1000, true};
        lists.setRowState(held, privateRow);
        lists.setRowState(user, {"PUBLIC_PRICE", "PUBLIC_CHANGE", "public evidence", {"완료 봉 대기"}, 1000, false});
        QCOMPARE(lists.rowState(held).price, QString("FAKE_PRIVATE_PRICE"));
        QCOMPARE(lists.publicState(), publicOnly);
        QVERIFY(!lists.publicState().contains("FAKE_PRIVATE"));
        QSignalSpy changed(&lists, &WatchlistWorkspaceList::changed);
        lists.setHoldings({held}); lists.setRowState(held, privateRow);
        QCOMPARE(changed.size(), 0);
        lists.clearPrivate();
        QVERIFY(lists.holdings().isEmpty());
        QVERIFY(lists.rowState(held).price.isEmpty());
        QCOMPARE(lists.rowState(user).price, QString("PUBLIC_PRICE"));
        QCOMPARE(lists.publicState(), publicOnly);
        QCOMPARE(lists.userList(id).entries.size(), 1);
        const auto count = changed.size(); lists.clearPrivate(); QCOMPARE(changed.size(), count);
        auto hostile = QJsonDocument::fromJson(publicOnly).object();
        hostile["holdings"] = QJsonArray{"FAKE_PRIVATE_ACCOUNT"};
        QVERIFY(!lists.restorePublicState(QJsonDocument(hostile).toJson()));
        QCOMPARE(lists.publicState(), publicOnly);
        hostile = QJsonDocument::fromJson(publicOnly).object();
        auto entries = hostile["lists"].toArray(); auto entry = entries[0].toObject();
        entry["account"] = "FAKE_PRIVATE_ACCOUNT"; entries[0] = entry; hostile["lists"] = entries;
        QVERIFY(!lists.restorePublicState(QJsonDocument(hostile).toJson()));
        QCOMPARE(lists.publicState(), publicOnly);
    }
    void userListChangesPreserveOrderAndValidateMetadata() {
        WatchlistWorkspaceList lists;
        const WatchlistIdentity first{"coin", "upbit", "KRW-BTC"}, second{"coin", "binance", "BTCUSDT"};
        const auto a = lists.createList("합성 A"), b = lists.createList("합성 B");
        QVERIFY(lists.renameList(a, "이름 변경"));
        QVERIFY(lists.setFlag(a, "purple"));
        QVERIFY(lists.moveList(a, 1)); QCOMPARE(lists.userLists()[1].id, a);
        const auto x = lists.addSection(a, "코인"), y = lists.addSection(a, "기타");
        QVERIFY(lists.renameSection(a, x, "공개 코인"));
        QVERIFY(lists.moveSection(a, x, 1)); QCOMPARE(lists.userList(a).sections[1].id, x);
        QVERIFY(lists.addIdentity(a, first, x)); QVERIFY(lists.addIdentity(a, second, y));
        QVERIFY(lists.moveIdentity(a, first, 1, y));
        QCOMPARE(lists.userList(a).entries[1].identity, first); QCOMPARE(lists.userList(a).entries[1].sectionId, y);
        QVERIFY(lists.removeSection(a, y));
        QCOMPARE(lists.userList(a).entries.size(), 2);
        QVERIFY(lists.userList(a).entries[0].sectionId.isEmpty()); QVERIFY(lists.userList(a).entries[1].sectionId.isEmpty());
        lists.useList(a); lists.useList(b); lists.useList(a); lists.useList("holdings");
        QCOMPARE(lists.recentLists(), QStringList({a, b}));
        QSignalSpy changed(&lists, &WatchlistWorkspaceList::changed);
        QVERIFY(lists.renameList(a, "이름 변경")); QVERIFY(lists.setFlag(a, "purple")); lists.useList(a);
        QCOMPARE(changed.size(), 0);
        const auto before = lists.publicState();
        QVERIFY(lists.createList(QString(101, 'x')).isEmpty()); QVERIFY(!lists.setFlag(a, "not-a-flag"));
        QVERIFY(!lists.moveList(a, -1)); QVERIFY(!lists.addIdentity(a, first));
        QVERIFY(!lists.addIdentity(a, {"stock", "US", "NVDA"}));
        QVERIFY(!lists.addIdentity(a, {"coin", "upbit", "KRW-BTC\n"}));
        QCOMPARE(lists.publicState(), before);
        QVERIFY(lists.removeIdentity(a, first)); QCOMPARE(lists.userList(a).entries.size(), 1);
        QVERIFY(lists.removeList(b)); QCOMPARE(lists.recentLists(), QStringList({a}));
    }
    void verifiedKoreanAlphanumericCodesRemainDistinct() {
        WatchlistWorkspaceList lists; const auto id = lists.createList("공식 코드 합성 목록");
        const WatchlistIdentity alpha{"stock", "KR", "0101N0"};
        QVERIFY(!lists.addIdentity(id, alpha));
        lists.setVerifiedInstruments({alpha});
        QVERIFY2(lists.addIdentity(id, alpha), "Verified Korean broker codes can contain letters");
        QCOMPARE(lists.userList(id).entries.first().identity.key(), QString("stock:KR:0101N0"));
    }
    void bithumbIdentityAndCsvKeepTheProvider() {
        WatchlistWorkspaceList lists; const auto id = lists.createList("빗썸 합성 공개 목록");
        const WatchlistIdentity bithumb{"coin", "bithumb", "KRW-BTC"}, upbit{"coin", "upbit", "KRW-BTC"};
        QVERIFY2(lists.addIdentity(id, bithumb), "Official Bithumb market codes retain their provider identity");
        QVERIFY(lists.addIdentity(id, upbit)); QCOMPARE(lists.userList(id).entries.size(), 2);
        WatchlistWorkspaceList restored; const auto copied = restored.importCsv(lists.exportCsv(id));
        QVERIFY(!copied.isEmpty()); QCOMPARE(restored.userList(copied).entries.size(), 2);
        QCOMPARE(restored.userList(copied).entries[0].identity, bithumb);
        QCOMPARE(restored.userList(copied).entries[1].identity, upbit);
        const auto ranked = WatchlistWorkspaceList::rankRealtime({bithumb}, {upbit}, {}, {}, 2);
        QCOMPARE(ranked.size(), 2);
    }
    void rowBatchesValidateAtomicallyAndEmitOnce() {
        WatchlistWorkspaceList lists;
        const WatchlistIdentity a{"coin", "upbit", "KRW-BTC"}, b{"coin", "binance", "BTCUSDT"};
        const QHash<QString, WatchlistRowState> rows{{a.key(), {"100", "+1", "public a", {"완료 봉 대기"}, 1000, false}},
            {b.key(), {"101", "+2", "public b", {}, 1000, false}}};
        QSignalSpy changed(&lists, &WatchlistWorkspaceList::changed);
        lists.setRowStates(rows); QCOMPARE(changed.size(), 1);
        QCOMPARE(lists.rowState(a).price, QString("100")); QCOMPARE(lists.rowState(b).price, QString("101"));
        lists.setRowStates(rows); QCOMPARE(changed.size(), 1);
        auto invalid = rows; invalid.insert("invalid:key", {"PRIVATE_SHOULD_NOT_APPEAR", {}, {}, {}, 0, false});
        auto altered = rows.value(a.key()); altered.price = "999"; invalid[a.key()] = altered;
        lists.setRowStates(invalid); QCOMPARE(changed.size(), 1); QCOMPARE(lists.rowState(a).price, QString("100"));
    }
    void csvRoundTripIsBoundedFormulaSafeAndPublicOnly() {
        WatchlistWorkspaceList lists;
        const QString name = "=SUM(1,2) \"합성\"\n새줄", sectionName = "+합성,\"섹션\"\n줄";
        const auto id = lists.createList(name, "red"); const auto section = lists.addSection(id, sectionName);
        const WatchlistIdentity coin{"coin", "upbit", "KRW-BTC"}, stock{"stock", "US", "NVDA"};
        lists.setVerifiedInstruments({stock}); QVERIFY(lists.addIdentity(id, coin, section)); QVERIFY(lists.addIdentity(id, stock));
        lists.setHoldings({{"coin", "binance", "ETHUSDT"}});
        lists.setRowState(coin, {"FAKE_PRIVATE_PRICE", {}, "FAKE_PRIVATE_ALIAS", {"FAKE_PRIVATE_BADGE"}, 1000, true});
        const auto csv = lists.exportCsv(id); QVERIFY2(!csv.isEmpty(), "Public lists can be exported as CSV");
        QVERIFY(csv.contains("\r\n")); QVERIFY(csv.contains("\"'=SUM")); QVERIFY(!csv.contains("FAKE_PRIVATE")); QVERIFY(!csv.contains("ETHUSDT"));
        WatchlistWorkspaceList imported; const auto before = imported.publicState(); QString error;
        QVERIFY(imported.importCsv(csv, &error).isEmpty()); QVERIFY(!error.isEmpty()); QCOMPARE(imported.publicState(), before);
        imported.setVerifiedInstruments({stock});
        const auto copied = imported.importCsv(csv, &error); QVERIFY2(!copied.isEmpty(), qPrintable(error));
        const auto restored = imported.userList(copied);
        QCOMPARE(restored.name, name); QCOMPARE(restored.flag, QString("red")); QCOMPARE(restored.sections.first().name, sectionName);
        QCOMPARE(restored.entries.size(), 2); QCOMPARE(restored.entries[0].identity, coin); QCOMPARE(restored.entries[1].identity, stock);
        QCOMPARE(restored.entries[0].sectionId, restored.sections.first().id); QVERIFY(restored.entries[1].sectionId.isEmpty());
        const auto stable = imported.publicState();
        auto extra = csv; extra.replace("\"code\"\r\n", "\"code\",\"account\"\r\n");
        QVERIFY(imported.importCsv(extra).isEmpty());
        QVERIFY(imported.importCsv("kind,list_name,flag,section,market,source,code\r\n\"unterminated").isEmpty());
        QVERIFY(imported.importCsv(QByteArray(1024 * 1024 + 1, 'x')).isEmpty());
        QVERIFY(imported.importCsv(QByteArray(1, char(0xff))).isEmpty()); QCOMPARE(imported.publicState(), stable);
        imported.setVerifiedInstruments({}); QVERIFY(!imported.isCurrentlyVerified(stock));
        QCOMPARE(imported.userList(copied).entries.size(), 2); QCOMPARE(imported.publicState(), stable);
        const auto empty = lists.createList("빈 합성 리스트");
        QVERIFY(!imported.importCsv(lists.exportCsv(empty)).isEmpty());
    }
    void legacyMigrationIsOneTimeAndWaitsForStockMetadata() {
        const QMap<QString, QStringList> original{{"coin", {"BTC", "BTC"}}, {"kr", {"0101N0"}}, {"us", {"NVDA"}}};
        const auto unchanged = original; WatchlistWorkspaceList lists;
        lists.setVerifiedInstruments({{"stock", "US", "NVDA"}});
        QCOMPARE(lists.migrateLegacy(original), 1);
        QCOMPARE(lists.userLists().size(), 2);
        int entries = 0; QString coinList;
        for (const auto& list : lists.userLists()) {
            entries += list.entries.size();
            if (list.name == "CRYPTO") { coinList = list.id; QCOMPARE(list.entries.first().identity.key(), QString("coin:upbit:KRW-BTC")); }
        }
        QCOMPARE(entries, 2); QVERIFY(!coinList.isEmpty());
        const auto once = lists.publicState(); QCOMPARE(lists.migrateLegacy(original), 1); QCOMPARE(lists.publicState(), once);
        WatchlistWorkspaceList reopened; QVERIFY(reopened.restorePublicState(once));
        reopened.setVerifiedInstruments({{"stock", "KR", "0101N0"}, {"stock", "US", "NVDA"}});
        QCOMPARE(reopened.migrateLegacy(original), 0); QCOMPARE(reopened.userLists().size(), 3);
        QVERIFY(reopened.renameList(coinList, "이름 바꾼 이전 리스트"));
        const auto complete = reopened.publicState(); QCOMPARE(reopened.migrateLegacy(original), 0); QCOMPARE(reopened.publicState(), complete);
        QCOMPARE(original, unchanged);
    }
    void realtimeBudgetKeepsVisibleThenSelectionThenHoldings() {
        const WatchlistIdentity a{"coin", "upbit", "KRW-BTC"}, b{"coin", "binance", "BTCUSDT"},
            c{"stock", "KR", "005930"}, d{"stock", "US", "NVDA"};
        const auto ranked = WatchlistWorkspaceList::rankRealtime({a, a}, {b, a}, {c}, {d}, 3);
        QCOMPARE(ranked.size(), 3); QCOMPARE(ranked[0], a); QCOMPARE(ranked[1], b); QCOMPARE(ranked[2], c);
        QCOMPARE(WatchlistWorkspaceList::rankRealtime({a}, {b}, {c}, {d}, 4).last(), d);
        QVERIFY(WatchlistWorkspaceList::rankRealtime({a}, {}, {}, {}, 0).isEmpty());
        QCOMPARE(WatchlistWorkspaceList::rankRealtime({{"coin", "upbit", "KRW-BTC\n"}, a}, {}, {}, {}, 3).size(), 1);
    }
};
QTEST_GUILESS_MAIN(WatchlistWorkspaceListTest)
#include "tst_watchlist_workspace_list.moc"
