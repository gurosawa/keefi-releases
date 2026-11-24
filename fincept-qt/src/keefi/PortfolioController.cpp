#include "keefi/IdentityCompatibility.h"
#include "keefi/PortfolioController.h"
#include "keefi/PublicKrwPrices.h"
#include "keefi/AccountModel.h"
#include "keefi/AssetsLedger.h"
#include "keefi/TradingController.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/FuturesTradingController.h"
#include "keefi/WalletReadController.h"
#include "keefi/OkxFuturesReadController.h"
#include "keefi/HyperliquidReadController.h"
#include "keefi/TradingTransport.h"
#include "keefi/SolanaReadRpc.h"
#include "keefi/Vault.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/BinanceBalanceStream.h"
#include "keefi/BinanceEarnCollector.h"
#include "keefi/FundFlowCollector.h"
#include "keefi/PortfolioCheckpoint.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QSet>
#include <map>
#include <algorithm>

namespace keefi {
namespace {
// readWindow returns snapshots and quotes ordered by their observation times.
// Split once so yesterday cannot enter today's chart, opening record, or coin P&L.
AssetsLedger::HistoryWindow takePreviousDay(AssetsLedger::HistoryWindow& window, qint64 midnight) {
    AssetsLedger::HistoryWindow previous;
    const auto takeEarlier = [midnight](auto& rows, auto timestamp) {
        const auto boundary = std::lower_bound(rows.cbegin(), rows.cend(), midnight,
            [&](const auto& row, qint64 at) { return timestamp(row) < at; }) - rows.cbegin();
        const auto earlier = rows.first(boundary);
        rows = rows.sliced(boundary);
        return earlier;
    };
    previous.snapshots = takeEarlier(window.snapshots, [](const auto& row) { return row.timeMs; });
    previous.snapshots.removeIf([midnight](const auto& row){
        return row.evidence.receivedAtMs >= midnight || row.evidence.sourceTimeMs >= midnight;
    });
    previous.quotes = takeEarlier(window.quotes, [](const auto& row) { return row.evidence.receivedAtMs; });
    return previous;
}

PortfolioCheckpoint previousDayClose(const AssetsLedger::HistoryWindow& previous, const CheckpointSelection& selection) {
    QMap<QString,QString> bindings;
    for (const auto& connection : selection.connections) bindings.insert(connection.connectionId, connection.binding);
    // Historical snapshots of a removed registration must never silently turn
    // yesterday's total into the subset that happens to be connected today.
    for (const auto& snapshot : previous.snapshots)
        if (!bindings.contains(snapshot.connectionId) || bindings.value(snapshot.connectionId) != snapshot.connectionBinding) return {};
    // Test the latest recorded instant first. Reads and quotes older than their
    // freshness horizon cannot contribute to a complete historical valuation.
    for (qsizetype end = previous.snapshots.size(); end > 0;) {
        const auto at = previous.snapshots[end-1].timeMs;
        const auto groupEnd=end;
        bool observed=false;
        while (end > 0 && previous.snapshots[end-1].timeMs == at) {
            const auto& evidence=previous.snapshots[--end].evidence;
            observed |= evidence.receivedAtMs<=at && evidence.sourceTimeMs<=at;
        }
        if (!observed) continue;
        const auto since = at - DisplayFreshnessMs;
        const auto first = std::lower_bound(previous.snapshots.cbegin(), previous.snapshots.cbegin()+groupEnd, since,
            [](const auto& row, qint64 t) { return row.timeMs < t; }) - previous.snapshots.cbegin();
        const auto quotesBegin = std::lower_bound(previous.quotes.cbegin(), previous.quotes.cend(), since,
            [](const auto& row, qint64 t) { return row.evidence.receivedAtMs < t; });
        const auto quotesEnd = std::upper_bound(quotesBegin, previous.quotes.cend(), at,
            [](qint64 t, const auto& row) { return t < row.evidence.receivedAtMs; });
        const auto point = checkpointAt(previous.snapshots.sliced(first,groupEnd-first),
            previous.quotes.sliced(quotesBegin-previous.quotes.cbegin(),quotesEnd-quotesBegin), selection, at);
        if (point.available && point.valuation.complete && point.valuation.issues.isEmpty()) return point;
    }
    return {};
}

bool sameClosingObservation(AssetValuationResult previous, const AssetValuationResult& closing) {
    // The clock advances on every UI tick. Compare the freshly evaluated rows,
    // including the actual quote and cross-rate evidence even if amounts match.
    previous.timeMs = closing.timeMs;
    return previous == closing;
}

PortfolioContributions previousDayChange(const PortfolioCheckpoint& opening, const AssetValuationResult& closing,
                                        const QList<AssetSnapshot>& closingSnapshots, const PortfolioDayHistory* last = nullptr) {
    if (!opening.available || !closing.complete) return {};
    // The owner passes last only while the historical window, policy and
    // selection are unchanged. On complete evaluations, every included
    // component's original evidence was already known at the earlier clock.
    if (last && last->previousDay.comparison.available && last->previousDay.comparison.fromMs == opening.timeMs &&
        last->closingValuation.timeMs <= closing.timeMs && last->closingSnapshots == closingSnapshots &&
        sameClosingObservation(last->closingValuation, closing)) {
        auto result = last->previousDay;
        result.comparison.toMs = closing.timeMs;
        return result;
    }
    auto result = portfolioContributions(opening.valuation, opening.policyVersion, opening.usedSnapshots,
                                        closing, ValuationPolicyVersion, closingSnapshots);
    if (!result.comparison.available)
        result.comparison = compareCompletePortfolioTotals(opening.valuation, opening.policyVersion, closing, ValuationPolicyVersion);
    return result;
}
WalletReadSummary summarizeWalletRead(const AssetSnapshot& snapshot) {
    WalletReadSummary summary;
    summary.balanceComplete = snapshot.balanceComplete;
    summary.quantitiesConfirmed = snapshot.evidence.complete && !snapshot.components.isEmpty();
    QSet<QString> holdings;
    bool hasExpectedBalance = false;
    const bool solana = snapshot.market == "solana";
    for (const auto& component : snapshot.components) {
        const auto& quantity = component.quantity;
        const bool signedQuantity = !solana && (component.kind == AssetComponentKind::PositionNotional ||
                                                component.kind == AssetComponentKind::Equity);
        const auto magnitude = signedQuantity && quantity.startsWith('-') ? quantity.mid(1) : quantity;
        if (!component.evidence.complete || component.evidence.scope != snapshot.evidence.scope ||
            !decimal::valid(magnitude)) {
            summary.quantitiesConfirmed = false;
            continue;
        }
        if (solana && component.kind == AssetComponentKind::Holding) {
            hasExpectedBalance |= component.asset == "SOL";
            if (decimal::positive(quantity)) holdings.insert(component.asset);
        } else if (!solana && component.kind == AssetComponentKind::PositionNotional) {
            if (decimal::positive(magnitude)) ++summary.openPositions;
        } else if (!solana && component.asset == "USDC" &&
                   (component.kind == AssetComponentKind::Equity || component.kind == AssetComponentKind::Cash)) {
            summary.usdcBalance = quantity;
            hasExpectedBalance = true;
        } else {
            summary.quantitiesConfirmed = false;
        }
    }
    summary.heldAssets = holdings.size();
    summary.quantitiesConfirmed &= hasExpectedBalance;
    return summary;
}
} // namespace

struct PortfolioController::Private {
    // The scheduler still polls every 250 ms. Notify consumers on a visible
    // transition (including the end of a reconnect cooldown), not on a tick.
    struct PollStatus {
        QString state, stage, accountScope, storage, balanceStream;
        qint64 receivedAtMs = 0, elapsedSeconds = 0;
        bool active = false, canReconnect = false;
        bool operator==(const PollStatus&) const = default;
    };
    struct Entry {
        PollStatus lastPollStatus;
        std::optional<WalletReadSummary> walletRead;
        PortfolioConnection view;QString sharedWith,binding;QSet<QString> cycleScopes;bool queued=true,suspended=false,authenticateAttempted=false,stopRequested=false,automaticQueued=false;
        qint64 completedAtMs=-1,lastReadStartedAtMs=-1000;
        bool streamPending=false;
        QElapsedTimer age;QString beforeId;QList<BrokerAccountChoice> choices;
        QList<AssetOrderObservation> lastOrders;QList<AssetQuote> lastQuotes;
        std::unique_ptr<TradingController> spot;std::unique_ptr<BrokerTradingController> broker;
        std::unique_ptr<FuturesTradingController> futures;std::unique_ptr<WalletReadController> wallet;
        std::unique_ptr<OkxFuturesReadController> okx;std::unique_ptr<HyperliquidReadController> hyperliquid;
        bool contributesToPortfolio() const {
            return sharedWith.isEmpty() && !AccountModel::isProviderSuspended(view.account.provider);
        }
        bool busy()const{return okx?okx->busy():hyperliquid?hyperliquid->busy():spot?spot->busy():broker?broker->busy():futures?futures->busy():wallet?wallet->busy():false;}
        QString status()const{return okx?okx->status():hyperliquid?hyperliquid->status():spot?spot->status():broker?broker->status():futures?futures->status():wallet?wallet->status():QString{};}
        AssetCapture capture()const{return okx?okx->assetCapture():hyperliquid?hyperliquid->assetCapture():spot?spot->assetCapture():broker?broker->assetCapture():futures?futures->assetCapture():wallet?wallet->assetCapture():AssetCapture{};}
    };
    PortfolioController* q;Vault& vault;Factories factories;AssetsLedger latest,history;QTimer poll,automatic;QElapsedTimer scheduleClock;
    QMap<QString,qint64> nextAutomaticStart;
    bool enabled=false,synchronizing=false,comparisonResetPending=false;QString manifest;
    QMap<QString,QString> comparisonBindings; qint64 connectionChangeDay=0;std::map<QString,std::unique_ptr<Entry>> entries;
    QMap<QString,AssetSnapshot> current;QMap<QString,AssetQuote> prices;qint64 jointHistoryAt=0; // Last time every live connection joined history together.

    // Today-only inputs and historical endpoints are cached independently of
    // current prices. Nothing in this cache survives lock/selection reset.
    AssetsLedger::HistoryWindow dayWindow,previousWindow;PortfolioCheckpoint previousClose;PortfolioDayHistory dayResult,pastResult;
    QMap<qint64,AssetValuationResult> pastTailValues;
    QSet<QString> wantedExclusions,pastExclusions;
    qint64 cachedDay=0,loadedThrough=0,wantedAt=0;quint64 dayGeneration=0;
    bool windowReady=false,dayBusy=false,pastReady=false,pastScopeValid=false;
    QString dayReadError;

    QMap<QString,ProviderTotalObservation> providerTotals; // Current session only; cleared with the vault.
    std::unique_ptr<TradingTransport> priceTransport;std::unique_ptr<PublicKrwPrices> pricing;QTimer storageTimer;bool latestDirty=false;
    QString lastPriceLookupFailure;
    bool initialPriceCheckpoint=true,initialPriceBatch=false;
    QMap<QString,AssetCapture> pendingHistory;QStringList savingHistoryScopes;
    bool latestSaveFailed=false,historySaveFailed=false;
    bool balancePilot=false,balanceSuppressed=false;
    QString balanceConnection;
    std::unique_ptr<BinanceBalanceStream> balanceStream;
    std::unique_ptr<BinanceEarnCollector> earn;
    qint64 lastEarnAttemptMs=-600000;
    QMap<QString,qint64> earnScopeAttempts;
    QSet<QString> earnAuthenticationBlocked;
    QString activeEarnScope;
    // G9-C1-a: coin deposit/withdrawal collection and the coin-only joint record.
    std::unique_ptr<FundFlowCollector> flow;
    // Injected account transports mean a harness: never read real exchange history behind it.
    bool flowReads=true;
    qint64 lastFlowCheckMs=-600000,coinJointAt=0,lastCoinEvaluationMs=-600000;
    QMap<QString,qint64> flowScopeAttempts;QMap<QString,QString> flowErrors;QSet<QString> flowBlocked;QStringList flowQueue;
    CoinPerformanceView coinView;QTimer coinTimer;quint64 baselineReceipt=0;
    qint64 wall()const{return factories.wallClockMs?factories.wallClockMs():QDateTime::currentMSecsSinceEpoch();}
    static bool coinAccount(const AccountRecord& a){return a.environment=="live"&&(a.provider=="upbit"||(a.provider=="binance"&&(a.market=="spot"||a.market=="usdm_futures")));}
    void clearCoin(){coinView={};coinTimer.stop();coinJointAt=0;lastCoinEvaluationMs=-600000;emit q->coinPerformanceChanged();}
    // Each evaluation decodes the history ledger once; keep it off every UI tick.
    void scheduleCoin(int delay=500){
        const int wait=delay==0?0:int(qMax<qint64>(delay,10000-(now()-lastCoinEvaluationMs)));
        if(!coinTimer.isActive()||coinTimer.remainingTime()>wait)coinTimer.start(wait);
    }
    void evaluateCoin(){
        lastCoinEvaluationMs=now();
        if(!vault.isUnlocked()||synchronizing||comparisonResetPending){coinView={};emit q->coinPerformanceChanged();return;}
        // A successful subset must never define the user's configured coin scope.
        // Pending/new connections have no snapshot yet, so verify them separately.
        for(const auto& [id,e]:entries){
            if(!coinAccount(e->view.account))continue;
            const bool present=std::any_of(current.cbegin(),current.cend(),[&](const auto& s){
                return s.connectionId==id&&s.connectionBinding==e->binding&&CoinPerformance::inScope(s);});
            if(!present){
                coinView={};coinView.inScope=true;coinView.today.name="오늘";coinView.sinceStart.name="기록 시작 이후";
                coinView.today.reason=coinView.sinceStart.reason="모든 코인 계좌의 잔고를 확인한 뒤 계산할 수 있어요";
                coinView.today.reasons=coinView.sinceStart.reasons={coinView.today.reason};
                emit q->coinPerformanceChanged();return;
            }
        }
        QString error;auto stored=history.coinInputs(&error);
        CoinPerformanceInput in;in.history=std::move(stored.snapshots);in.quotes=std::move(stored.quotes);in.earn=std::move(stored.earn);
        in.flows=std::move(stored.flows);in.baseline=std::move(stored.baseline);in.current=current.values();in.flowErrors=flowErrors;in.nowMs=wall();
        if(windowReady){in.today=dayWindow.snapshots;in.quotes.append(dayWindow.quotes);}
        coinView=CoinPerformance::evaluate(in);
        if(!error.isEmpty()&&coinView.inScope){
            coinView.today.available=coinView.sinceStart.available=false;
            coinView.today.reason=coinView.sinceStart.reason=error;
            coinView.today.reasons=coinView.sinceStart.reasons=QStringList{error};
            coinView.proposedBaseline={};coinView.lines.clear();
        }
        // A first start is stored once; a later read keeps it instead of moving it forward.
        if(!coinView.proposedBaseline.isEmpty()&&!baselineReceipt&&history.isOpen()&&!history.isWriting()){
            static quint64 next=0;const auto receipt=++next;
            if(history.saveCoinBaseline(coinView.proposedBaseline,receipt,&error)==AssetsLedger::CollectionWrite::Pending)baselineReceipt=receipt;
        }
        emit q->coinPerformanceChanged();
    }
    Private(PortfolioController* owner,Vault& v,Factories f):q(owner),vault(v),factories(std::move(f)),
        latest(&v,QFileInfo(v.filePath()).dir().filePath("assets-current.keefi")),history(&v,QFileInfo(v.filePath()).dir().filePath("assets.keefi")){scheduleClock.start();}
    qint64 now()const{return factories.monotonicMs?factories.monotonicMs():scheduleClock.elapsed();}
    static PortfolioDayHistory unavailableDay(const QString& reason){
        PortfolioDayHistory result;result.baseline.reason=result.comparison.reason=reason;return result;
    }
    QString unavailableDayReason(qint64 at)const{
        if(!accountReadsAvailable(vault))return QStringLiteral("금고를 열면 표시합니다.");
        if(synchronizing||comparisonResetPending)return QStringLiteral("연결 변경을 반영하고 있습니다.");
        if(connectionChangeDay==portfolioHistoryDayStart(at))return QStringLiteral("오늘 연결이 추가·삭제되거나 인증정보가 바뀌어 비교할 수 없습니다.");
        return {};
    }
    CheckpointSelection daySelection(const QSet<QString>& excluded)const{
        CheckpointSelection selection;selection.excludedAssets=excluded;
        for(const auto& [id,e]:entries)if(e->contributesToPortfolio())selection.connections.append({id,e->binding});
        return selection;
    }
    AssetValuationResult currentDayValue(const CheckpointSelection& selection,qint64 at,QList<AssetSnapshot>* usedSnapshots=nullptr)const{
        if(usedSnapshots)usedSnapshots->clear();
        const auto point=checkpointAt(current.values(),prices.values(),selection,at);auto value=point.valuation;
        bool ready=true;for(const auto& [id,e]:entries)if(e->contributesToPortfolio() && (e->view.active||e->view.state!="complete"))ready=false;
        if(!ready||!point.available){value={};value.currency="KRW";value.timeMs=at;
            value.issues.append(!ready?QStringLiteral("현재 모든 연결의 잔고 조회가 완료되어야 비교할 수 있습니다."):point.reason);}
        else if(usedSnapshots)*usedSnapshots=point.usedSnapshots;
        return value;
    }
    QStringList blockedConnectionNames()const{
        QStringList names;
        for(const auto& [id,e]:entries){
            if(!e->contributesToPortfolio() || (!e->view.active&&e->view.state=="complete"))continue;
            const auto alias=e->view.account.alias.trimmed();
            if(!alias.isEmpty())names.append(alias); // Never substitute an opaque id, scope or credential.
        }
        return names;
    }
    void clearDayHistory(const QString& reason={}){
        ++dayGeneration;history.cancelWindowRead();dayBusy=windowReady=pastReady=pastScopeValid=false;
        dayWindow={};previousWindow={};previousClose={};dayResult=unavailableDay(reason);pastResult={};pastTailValues.clear();
        wantedExclusions.clear();pastExclusions.clear();cachedDay=loadedThrough=wantedAt=0;dayReadError.clear();
    }
    static PortfolioHistoryPoint historyPoint(qint64 at,const AssetValuationResult& value,QString reason,
        const PortfolioHistoryPoint* previous,const AssetValuationResult& previousValue){
        PortfolioHistoryPoint point;point.timeMs=at;
        if(reason.isEmpty()){
            if(!value.issues.isEmpty()||value.accountScopes.isEmpty())reason=QStringLiteral("계좌 조회 범위 또는 평가 입력의 문제가 있어 표시할 수 없습니다.");
            else if(value.knownSubtotal.isEmpty()||(!value.complete&&std::none_of(value.rows.cbegin(),value.rows.cend(),
                [](const auto& row){return row.included&&!row.value.isEmpty();})))reason=QStringLiteral("이 시각에 평가 가능한 자산이 없습니다.");
        }
        point.available=reason.isEmpty();point.reason=reason;
        if(point.available){
            point.value=value.knownSubtotal;point.complete=value.complete;
            if(previous&&previous->available){
                if(at-previous->timeMs>JointHistoryIntervalMs+DisplayFreshnessMs)point.reason=QStringLiteral("이 구간에 함께 기록된 자료가 없어 선을 잇지 않습니다.");
                else{const auto compared=comparePortfolioValues(previousValue,ValuationPolicyVersion,value,ValuationPolicyVersion);
                    point.connectToPrevious=compared.available;point.reason=compared.reason;}
            }
        }
        return point;
    }
    void preparePast(const CheckpointSelection& selection,qint64 at){
        // Historical points are immutable for this input window and exclusion
        // set. Re-evaluate them once; only the live endpoint changes each tick.
        AssetValuationResult empty;empty.currency="KRW";empty.timeMs=at;
        pastResult=portfolioDayHistory(dayWindow.snapshots,dayWindow.quotes,selection,empty,at);
        previousClose=previousDayClose(previousWindow,selection);
        pastScopeValid=!pastResult.points.isEmpty();pastTailValues.clear();
        if(pastScopeValid){
            pastResult.points.removeLast(); // Remove the synthetic live endpoint.
            const bool exact=std::any_of(dayWindow.snapshots.cbegin(),dayWindow.snapshots.cend(),[at](const auto& row){return row.timeMs==at;});
            if(exact){
                const auto checkpoint=checkpointAt(dayWindow.snapshots,dayWindow.quotes,selection,at);
                const auto previous=pastResult.points.isEmpty()?nullptr:&pastResult.points.last();
                const auto before=previous?checkpointAt(dayWindow.snapshots,dayWindow.quotes,selection,previous->timeMs).valuation:AssetValuationResult{};
                pastResult.points.append(historyPoint(at,checkpoint.valuation,checkpoint.reason,previous,before));
            }
            // At an exact stored timestamp the current endpoint replaces that
            // point; retain both possible preceding endpoint valuations.
            for(qsizetype i=qMax(qsizetype(0),pastResult.points.size()-2);i<pastResult.points.size();++i){
                const auto time=pastResult.points[i].timeMs;
                pastTailValues.insert(time,checkpointAt(dayWindow.snapshots,dayWindow.quotes,selection,time).valuation);
            }
        }
        pastExclusions=selection.excludedAssets;pastReady=true;
    }
    PortfolioDayHistory composeDay(qint64 at){
        if(!dayReadError.isEmpty())return unavailableDay(dayReadError);
        const auto selection=daySelection(wantedExclusions);
        const bool samePast=pastReady&&pastExclusions==wantedExclusions;
        if(!samePast)preparePast(selection,at);
        auto result=pastResult;if(!pastScopeValid)return result;
        QList<AssetSnapshot> usedSnapshots;const auto value=currentDayValue(selection,at,&usedSnapshots);
        result.closingValuation=value;result.closingSnapshots=usedSnapshots;
        result.previousDay=previousDayChange(previousClose,value,usedSnapshots,samePast?&dayResult:nullptr);
        if(result.baseline.available)result.comparison=comparePortfolioValues(result.baseline.valuation,result.baseline.policyVersion,value,ValuationPolicyVersion);
        else result.comparison.reason=result.baseline.reason;
        if(result.comparison.available)result.contributions=portfolioContributions(result.baseline.valuation,result.baseline.policyVersion,
            result.baseline.usedSnapshots,value,ValuationPolicyVersion,usedSnapshots);
        else result.contributions.reason=result.comparison.reason;
        if(!result.comparison.available)result.blockedConnectionNames=blockedConnectionNames();
        if(!result.points.isEmpty()&&result.points.last().timeMs==at)result.points.removeLast();
        const auto previous=result.points.isEmpty()?nullptr:&result.points.last();
        result.points.append(historyPoint(at,value,{},previous,previous?pastTailValues.value(previous->timeMs):AssetValuationResult{}));
        return result;
    }
    void clearBalanceStream(){
        // Disconnect before destruction: lock/reset must not publish a stale connection status.
        if(balanceStream)QObject::disconnect(balanceStream.get(),nullptr,q,nullptr);
        balanceStream.reset();balanceConnection.clear();
        for(auto& [id,e]:entries){e->streamPending=false;e->view.balanceStreamStatus.clear();}
    }
    void syncBalanceStream(){
        if(!balancePilot||!enabled||balanceSuppressed||!accountReadsAvailable(vault))return;
        if(balanceStream){
            auto* e=entry(balanceConnection);
            if(!e||e->suspended||e->view.state=="failed"||e->view.state=="stopped"){
                balanceSuppressed=true;balanceStream->stop();if(e)e->streamPending=false;
            }
            return;
        }
        // Pilot: one successful live Spot connection. Other connections keep their reviewed polling cadence.
        for(auto& [id,e]:entries)if(e->view.state=="complete"&&!e->suspended&&e->view.account.provider=="binance"&&
            e->view.account.market=="spot"&&e->view.account.environment=="live"){
            balanceConnection=id;
            balanceStream.reset(factories.binanceBalances?factories.binanceBalances(vault):new BinanceBalanceStream(vault));
            if(!balanceStream){balanceConnection.clear();return;}
            QObject::connect(balanceStream.get(),&BinanceBalanceStream::changed,q,[this]{
                if(auto* current=entry(balanceConnection);current){current->view.balanceStreamStatus=balanceStream->status();emit q->changed();}
            });
            QObject::connect(balanceStream.get(),&BinanceBalanceStream::balancesChanged,q,[this]{
                if(!enabled||balanceSuppressed||!accountReadsAvailable(vault))return;
                if(auto* current=entry(balanceConnection);current&&!current->suspended&&
                    (current->view.state=="complete"||current->view.state=="reading")){
                    if(!current->streamPending){current->streamPending=true;QTimer::singleShot(0,q,[this]{drive();});}
                }
            });
            balanceStream->start(balanceConnection);return;
        }
    }
    static int automaticInterval(const AccountRecord& a){
        // Only the reviewed spot read paths use the shorter cadence.
        return a.market=="spot"&&(a.provider=="binance"||a.provider=="upbit"||a.provider=="okx")?20000:60000;
    }
    void queueAutomaticReads(){
        if(!enabled||!accountReadsAvailable(vault))return;
        const auto at=now();
        // Coin deposit/withdrawal history: every 10 minutes per account, or now when asked.
        if(flow&&flowReads&&!flow->busy()&&at-lastFlowCheckMs>=15000){
            lastFlowCheckMs=at;QString error;
            for(const auto& s:current){
                if(!CoinPerformance::inScope(s)||flowBlocked.contains(s.accountScope))continue;
                if(!flowQueue.contains(s.accountScope)&&at-flowScopeAttempts.value(s.accountScope,-600000)<600000)continue;
                auto* e=entry(s.connectionId);
                if(!e||e->view.active||e->view.state!="complete"||e->suspended||e->binding!=s.connectionBinding)continue;
                // Only a due, usable coin snapshot authorizes a collection writer.
                if(history.isWriting()||(!history.isOpen()&&!history.open(true,&error)))break;
                flowScopeAttempts[s.accountScope]=at;flowQueue.removeAll(s.accountScope);
                if(flow->start(s))break;
            }
        }
        if(at-lastCoinEvaluationMs>=60000)scheduleCoin(0);
        // A first Earn read is explicit in the records window. Once tracking has
        // a durable state, resume while unlocked without creating keys or opening
        // another writer. Failed/unsupported responses never become PnL inputs.
        if(earn&&!earn->busy()&&history.isOpen()&&at-lastEarnAttemptMs>=60000){
            lastEarnAttemptMs=at; // Do not decode a large idle ledger every second.
            for(const auto& saved:history.earnStates()){
                if(earnAuthenticationBlocked.contains(saved.accountScope)||at-earnScopeAttempts.value(saved.accountScope,-600000)<600000)continue;
                auto* e=entry(saved.connectionId);
                if(!e||e->view.active||e->view.state!="complete"||e->suspended||e->binding!=saved.binding)continue;
                const auto s=current.constFind(saved.accountScope);
                if(s==current.cend()||s->connectionBinding!=saved.binding)continue;
                lastEarnAttemptMs=at;earn->start(*s);break;
            }
        }
        for(auto& [id,e]:entries)if(e->sharedWith.isEmpty()&&e->view.state=="complete"&&!e->suspended&&!e->view.active&&!e->queued&&
            e->completedAtMs>=0&&at-e->completedAtMs>=automaticInterval(e->view.account)){
            e->queued=true;e->automaticQueued=true;
        }
        drive();
    }
    QString bindingFor(const AccountRecord& account){
        QCryptographicHash digest(QCryptographicHash::Sha256);digest.addData(identityDomain(IdentityDomain::CurrentAssetsCredentialBinding));
        for(const auto& part:{account.provider,account.environment,account.market,account.region}){const auto value=part.toUtf8();digest.addData(QByteArray::number(value.size()));digest.addData(":");digest.addData(value);}
        const bool ok=vault.withCredentials(account.id,[&](const CredentialSet& credentials){for(const auto& [name,value]:credentials){
            digest.addData(name.toUtf8());digest.addData(":");digest.addData(QByteArray::number(value.size()));digest.addData(":");digest.addData(QByteArrayView(reinterpret_cast<const char*>(value.data()),qsizetype(value.size())));
        }});return ok?QString::fromLatin1(digest.result().toHex()):QString{};
    }
    QMap<QString,QString> connectionBindings(){
        QMap<QString,QString> result;
        for(const auto& account:vault.accounts())if(account.provider!="jupiter" && !AccountModel::isProviderSuspended(account.provider))result.insert(account.id,bindingFor(account));
        return result;
    }
    QString canonical(const QString& id)const{const auto i=entries.find(id);return i!=entries.end()&&!i->second->sharedWith.isEmpty()?i->second->sharedWith:id;}
    Entry* entry(const QString& id){const auto i=entries.find(canonical(id));return i==entries.end()?nullptr:i->second.get();}
    void clear(){synchronizing=true;if(flow)flow->stop();flowScopeAttempts.clear();flowErrors.clear();flowBlocked.clear();flowQueue.clear();lastFlowCheckMs=-600000;coinJointAt=0;coinView={};coinTimer.stop();baselineReceipt=0;if(earn)earn->stop();earnAuthenticationBlocked.clear();earnScopeAttempts.clear();activeEarnScope.clear();lastEarnAttemptMs=-600000;clearBalanceStream();balanceSuppressed=false;poll.stop();automatic.stop();storageTimer.stop();latestDirty=false;initialPriceCheckpoint=true;initialPriceBatch=false;latestSaveFailed=historySaveFailed=false;pendingHistory.clear();savingHistoryScopes.clear();if(pricing)pricing->clear();emit q->aboutToReset();entries.clear();nextAutomaticStart.clear();current.clear();providerTotals.clear();prices.clear();jointHistoryAt=0;manifest.clear();comparisonBindings.clear();comparisonResetPending=false;latest.lock();history.lock();synchronizing=false;emit q->assetsChanged();emit q->changed();}
    void flushStorage(){
        if(!vault.isUnlocked())return;QString error;
        if(latestDirty&&!latest.isWriting()){
            latestDirty=false;
            if(!latest.open(true,&error)||!latest.replaceLatest(current.values(),prices.values(),&error,true)){
                latestSaveFailed=true;
                for(auto& [id,e]:entries)if(e->view.receivedAtMs>0)e->view.storageStatus="최신 조회 성공 · 마지막 자료 저장 실패";
            }
            // Serializing both ledgers in one event-loop turn stalls input; history follows in the next turn.
            if(!pendingHistory.isEmpty()){storageTimer.start(0);return;}
        }
        if(!pendingHistory.isEmpty()&&!history.isWriting()){
            QList<AssetSnapshot> snapshots;QList<AssetOrderObservation> orders;savingHistoryScopes.clear();
            for(auto it=pendingHistory.cbegin();it!=pendingHistory.cend();++it){const auto& c=it.value();if(!c.snapshot.id.isEmpty())snapshots.append(c.snapshot);orders.append(c.orders);savingHistoryScopes.append(it.key());}
            pendingHistory.clear();
            if(!history.open(true,&error)||!history.appendWithArchive(snapshots,{},prices.values(),orders,&error,true)){
                historySaveFailed=true;
                jointHistoryAt=0;savingHistoryScopes.clear();
                for(auto& [id,e]:entries)e->view.storageStatus+=" · 과거 기록 저장 실패";
            }
        }
    }
    QString quoteKey(const AssetQuote& p)const{return p.baseAsset+":"+p.quoteAsset+":"+p.evidence.source+":"+p.evidence.scope;}
    void queuePrices(){if(enabled&&pricing&&accountReadsAvailable(vault))pricing->refresh(current.values());}
    void synchronize(){
        if(!accountReadsAvailable(vault)){clear();return;}if(synchronizing)return;synchronizing=true;
        QString next;const auto records=vault.accounts();for(const auto& a:records)next+=a.id+":"+a.credentialRef+":"+a.provider+":"+a.environment+":"+a.region+";";
        if(next==manifest){synchronizing=false;return;}clearBalanceStream();balanceSuppressed=false;if(pricing)pricing->clear();providerTotals.clear();emit q->aboutToReset();entries.clear();current.clear();prices.clear();pendingHistory.clear();jointHistoryAt=0;initialPriceCheckpoint=true;initialPriceBatch=false;manifest=next;
        QMap<QString,QString> brokerGroups;
        for(const auto& a:records){if(a.provider=="jupiter")continue;auto item=std::make_unique<Entry>();item->view.account=a;
            if (AccountModel::isProviderSuspended(a.provider)) {
                item->queued = false;
                item->view.state = "unsupported";
                item->view.stage = "이 증권사의 연결은 비활성화되어 있습니다. 저장한 연결은 삭제할 수 있습니다.";
                entries.emplace(a.id,std::move(item));
                continue;
            }
            item->binding=bindingFor(a);
            if(AccountModel::retiredEnvironment(a)){ // Kept so an older vault opens; never read.
                item->queued=false;item->view.state="unsupported";item->view.stage="지원이 끝난 모의·테스트넷 연결입니다. 계좌·연결에서 삭제하세요.";
                entries.emplace(a.id,std::move(item));continue;
            }
            const auto group=a.provider+":"+a.environment+":"+a.credentialRef;
            if(a.market=="domestic_equity"&&!a.credentialRef.isEmpty()&&brokerGroups.contains(group)){
                item->sharedWith=brokerGroups.value(group);item->view.shared=true;item->queued=false;
            }else{
                TradingTransport* transport=factories.trading?factories.trading(a):nullptr;
                if(a.provider=="okx"&&a.region=="global"&&(a.market=="spot"||a.market=="okx_futures")){
                    // Every Global registration reads the same unified equity and all derivative families.
                    // Two keys or market aliases for the same verified UID share one snapshot scope.
                    item->okx=std::make_unique<OkxFuturesReadController>(vault,transport);item->okx->selectAccount(a.id);
                }else if(a.provider=="phantom"&&a.market=="hyperliquid_perps"){
                    item->hyperliquid=std::make_unique<HyperliquidReadController>(vault,transport);item->hyperliquid->selectAccount(a.id);
                }else if(a.market=="spot") {item->spot=transport?std::make_unique<TradingController>(vault,transport):std::make_unique<TradingController>(vault);
                    item->spot->setAutomaticRefresh(false);item->spot->selectAccount(a.id);}
                else if(a.market=="usdm_futures"){item->futures=transport?std::make_unique<FuturesTradingController>(vault,transport):std::make_unique<FuturesTradingController>(vault);item->futures->selectAccount(a.id);}
                else if(a.market=="domestic_equity"){item->broker=std::make_unique<BrokerTradingController>(vault,transport);item->broker->setAutomaticRefresh(false);item->broker->selectConnection(a.id);
                    const auto identity=item->broker->credentialSessionIdentity();const auto sessionGroup="session:"+a.provider+":"+a.environment+":"+identity;
                    if(a.provider=="toss"&&!identity.isEmpty()&&brokerGroups.contains(sessionGroup)){item->sharedWith=brokerGroups.value(sessionGroup);item->view.shared=true;item->queued=false;item->broker.reset();}
                    else{brokerGroups[group]=a.id;if(a.provider=="toss"&&!identity.isEmpty())brokerGroups[sessionGroup]=a.id;}}
                else if(a.provider=="phantom"){auto* t=factories.wallet?factories.wallet(a):nullptr;item->wallet=std::make_unique<WalletReadController>(vault,t);item->wallet->selectAccount(a.id);}
                else{item->queued=false;item->view.state="unsupported";item->view.stage="자산 조회 미지원 연결";}
                QObject* source=item->okx?static_cast<QObject*>(item->okx.get()):item->hyperliquid?static_cast<QObject*>(item->hyperliquid.get()):item->spot?static_cast<QObject*>(item->spot.get()):item->broker?static_cast<QObject*>(item->broker.get()):item->futures?static_cast<QObject*>(item->futures.get()):static_cast<QObject*>(item->wallet.get());
                if(source){const auto changed=[this]{QTimer::singleShot(0,q,[this]{drive(true);});};
                    if(item->okx){
                        QObject::connect(item->okx.get(),&OkxFuturesReadController::changed,q,changed);
                        QObject::connect(item->okx.get(),&OkxFuturesReadController::identityChanged,q,[this,id=a.id](const QString& oldScope,const QString& verifiedScope){
                            bool removed=false;
                            for(auto it=current.begin();it!=current.end();){
                                if((!oldScope.isEmpty()&&it.key()==oldScope)||
                                    (it->connectionId==id&&it.key()!=verifiedScope)){it=current.erase(it);removed=true;}
                                else ++it;
                            }
                            if(removed){latestDirty=true;storageTimer.start(200);emit q->assetsChanged();}
                        });
                    }
                    if(item->hyperliquid)QObject::connect(item->hyperliquid.get(),&HyperliquidReadController::changed,q,changed);
                    if(item->spot)QObject::connect(item->spot.get(),&TradingController::changed,q,changed);
                    if(item->broker)QObject::connect(item->broker.get(),&BrokerTradingController::changed,q,changed);
                    if(item->futures)QObject::connect(item->futures.get(),&FuturesTradingController::changed,q,changed);
                    if(item->wallet)QObject::connect(item->wallet.get(),&WalletReadController::changed,q,changed);
                }
            }entries.emplace(a.id,std::move(item));
        }
        const auto acceptsSaved=[this](const AssetSnapshot& s){
            const auto* e=entry(s.connectionId);
            // Earlier spot-only OKX caches used a different scope and must never be added to unified equity.
            return e && !AccountModel::isProviderSuspended(e->view.account.provider) &&
                !s.connectionBinding.isEmpty() && s.connectionBinding==e->binding &&
                (!e->okx||s.market=="okx_unified")&&(!e->hyperliquid||s.market=="hyperliquid_perps");
        };
        QString error;if(vault.isUnlocked()&&latest.exists()&&latest.open(false,&error)){
            for(const auto& s:latest.snapshots())if(acceptsSaved(s))current[s.accountScope]=s;
            for(const auto& p:latest.quotes())prices[quoteKey(p)]=p;
        }else if(vault.isUnlocked()&&!latest.exists()&&history.exists()&&history.open(false,&error)){
            for(const auto& s:history.snapshots())if(acceptsSaved(s)&&(!current.contains(s.accountScope)||current[s.accountScope].timeMs<s.timeMs))current[s.accountScope]=s;
            for(const auto& p:history.quotes())prices[quoteKey(p)]=p;
        }
        comparisonBindings=connectionBindings();comparisonResetPending=false;
        synchronizing=false;
        poll.start();if(enabled)automatic.start();emit q->assetsChanged();emit q->changed();
        // Restored holdings already identify the required price routes. Start
        // those alongside account reads and consume any warm public quote cache
        // without waiting for the first authenticated balance response. Publish
        // the stored inputs first so a dated saved view can preserve its basis.
        if(!current.isEmpty())queuePrices();
        if(enabled)drive();
    }
    bool record(Entry& e,const AssetCapture& capture){
        if(capture.snapshot.id.isEmpty())return false;
        const bool totalChanged=capture.providerTotal && providerTotals.value(e.view.account.id)!=*capture.providerTotal;
        if(totalChanged)providerTotals[e.view.account.id]=*capture.providerTotal;
        const bool newSnapshot=capture.snapshot.id!=e.beforeId,ordersChanged=capture.orders!=e.lastOrders;
        if(!newSnapshot&&!ordersChanged&&capture.quotes==e.lastQuotes)return totalChanged;
        e.lastOrders=capture.orders;e.lastQuotes=capture.quotes;
        if (newSnapshot && (e.wallet || e.hyperliquid)) {
            // Cache only the response accepted for this specific registration.
            // An address can occur in several registrations sharing a history scope.
            e.walletRead.reset();
            if (capture.snapshot.connectionId == e.view.account.id && capture.snapshot.market == e.view.account.market)
                e.walletRead = summarizeWalletRead(capture.snapshot);
        }
        auto snapshot=capture.snapshot;snapshot.connectionId=e.view.account.id;snapshot.connectionBinding=e.binding;
        if(!e.broker)for(auto it=current.begin();it!=current.end();)if(it->connectionId==snapshot.connectionId&&it.key()!=snapshot.accountScope)it=current.erase(it);else ++it;
        e.cycleScopes.insert(snapshot.accountScope);e.view.accountScope=snapshot.accountScope;current[snapshot.accountScope]=snapshot;e.beforeId=snapshot.id;e.view.receivedAtMs=snapshot.timeMs;
        for(const auto& p:capture.quotes)prices[quoteKey(p)]=p;
        queuePrices();
        if(!vault.isUnlocked())return true;
        latestDirty=true;e.view.storageStatus="최신 조회 성공 · 마지막 자료 저장 대기";
        const auto at=QDateTime::currentMSecsSinceEpoch();
        // History joins every live connection at one instant so a later checkpoint can
        // value them together under the 180 s rule; one account alone never samples.
        QList<AssetSnapshot> live;bool everyConnection=true;
        for(const auto& [id,other]:entries){if(!other->contributesToPortfolio())continue;
            // record() runs just before this entry becomes complete. A broker
            // with more account pages, or any other loading/failed entry, is not ready.
            const bool completing=other.get()==&e&&newSnapshot&&!e.busy()&&(!e.broker||e.choices.isEmpty())&&snapshot.balanceComplete;
            if(!completing&&(other->view.active||other->view.state!="complete"))everyConnection=false;
            bool seen=false;
            for(const auto& s:current)if(s.connectionId==other->view.account.id){live.append(s);seen=true;}if(!seen)everyConnection=false;}
        if(newSnapshot&&everyConnection&&jointHistoryDue(live,at,jointHistoryAt)){for(const auto& s:live)pendingHistory[s.accountScope].snapshot=s;jointHistoryAt=coinJointAt=at;}
        else if(newSnapshot&&CoinPerformance::inScope(snapshot)){
            // Coin accounts alone, when another connection keeps the full record from forming.
            QList<AssetSnapshot> coin;bool everyCoin=true;
            for(const auto& [id,other]:entries){if(!other->sharedWith.isEmpty()||!coinAccount(other->view.account))continue;
                bool seen=false;for(const auto& s:current)if(s.connectionId==id&&s.connectionBinding==other->binding&&CoinPerformance::inScope(s)){coin.append(s);seen=true;}
                if(!seen)everyCoin=false;
                const bool completing=other.get()==&e&&!e.busy()&&snapshot.balanceComplete;
                if(!completing&&(other->view.active||other->view.state!="complete"))everyCoin=false;}
            if(everyCoin&&jointHistoryDue(coin,at,coinJointAt)){for(const auto& s:coin)pendingHistory[s.accountScope].snapshot=s;coinJointAt=at;}
        }
        if(ordersChanged&&everyConnection)pendingHistory[snapshot.accountScope].orders.append(capture.orders);
        storageTimer.start(200);emit q->assetsChanged();return true;
    }
    bool beginAssets(Entry& e){return e.okx?e.okx->refreshAssets():e.hyperliquid?e.hyperliquid->refreshAssets():e.spot?e.spot->refreshAssets():e.broker?e.broker->refreshAssets():e.futures?e.futures->refreshAssets():e.wallet?e.wallet->refresh():false;}
    void drive(bool sourceChanged=false){
        if(synchronizing||!accountReadsAvailable(vault))return;int active=0;QSet<QString> credentialActive;
        bool changed=sourceChanged;
        if(enabled&&!balanceSuppressed)for(auto& [id,e]:entries)
            if(e->streamPending&&e->view.state=="complete"&&!e->suspended&&!e->view.active&&
                !e->queued&&now()-e->lastReadStartedAtMs>=1000){e->queued=true;e->automaticQueued=true;}
        for(const auto& [id,e]:entries)if(e->suspended&&!e->view.account.credentialRef.isEmpty())credentialActive.insert(e->view.account.credentialRef);
        for(auto& [id,p]:entries){auto& e=*p;if(!e.sharedWith.isEmpty() || e.view.state=="unsupported")continue;
            if(e.view.active){e.view.elapsedMs=e.age.elapsed();e.view.stage=e.status();
                if(e.busy()){++active;credentialActive.insert(e.view.account.credentialRef);continue;}
                if(e.stopRequested||e.suspended){e.stopRequested=false;e.view.active=false;e.choices.clear();e.queued=false;e.view.state=e.suspended?"manual":"stopped";e.view.stage=e.suspended?"거래 화면에서 조회 중":"조회 중단";continue;}
                if(e.view.state=="authenticating"){
                    if(!e.broker->authenticated()){e.view.active=false;e.view.state="authentication_required";e.queued=false;continue;}
                    e.choices=e.broker->accountChoices();e.view.state="selecting";
                }
                if(e.view.state=="reading"){
                    const auto c=e.capture();const bool fresh=!c.snapshot.id.isEmpty()&&c.snapshot.id!=e.beforeId;changed |= record(e,c);
                    if(!fresh){e.view.active=false;e.view.state="failed";e.queued=false;continue;}
                    if(!e.broker||e.choices.isEmpty()||e.suspended){
                        if(e.broker&&!e.suspended)for(auto it=current.begin();it!=current.end();)if(it->connectionId==e.view.account.id&&!e.cycleScopes.contains(it.key())){it=current.erase(it);latestDirty=true;storageTimer.start(200);}else ++it;
                        emit q->assetsChanged();e.view.active=false;e.view.state="complete";e.queued=false;e.completedAtMs=now();continue;}
                    e.view.state="selecting";
                }
                if(e.view.state=="selecting"){
                    if(e.choices.isEmpty()){e.view.active=false;e.view.state="account_required";e.queued=false;continue;}
                    const auto handle=e.choices.first().handle;
                    if(e.broker->selectBrokerAccount(handle)&&beginAssets(e)){e.choices.removeFirst();e.view.state="reading";}
                    if(e.age.elapsed()>120000){e.view.active=false;e.view.state="failed";e.queued=false;e.view.stage="계좌 조회 준비 시간 초과";}
                }
                if(e.view.active){++active;credentialActive.insert(e.view.account.credentialRef);}continue;
            }
            if(e.suspended){const auto c=e.capture();if(!c.snapshot.id.isEmpty()){changed |= record(e,c);e.view.state="complete";}continue;}
            if(e.busy()){e.view.state="waiting_for_manual_request";e.view.stage="기존 요청 처리 후 자산 갱신 · "+e.status();continue;}
            if(!enabled||!e.queued||active>=3||(!e.view.account.credentialRef.isEmpty()&&credentialActive.contains(e.view.account.credentialRef)))continue;
            // Pace automatic starts across connections; provider-wide pacing is
            // deliberately stricter than per-key limits. Transport cooldowns
            // and each adapter's request pacing remain authoritative.
            const auto service=e.view.account.provider+":"+e.view.account.environment;
            if(e.automaticQueued){
                const auto at=now();if(at<nextAutomaticStart.value(service))continue;
                nextAutomaticStart[service]=at+1000;e.automaticQueued=false;
            }
            e.age.start();e.view.elapsedMs=0;e.cycleScopes.clear();e.lastReadStartedAtMs=now();e.streamPending=false;
            if(e.broker){
                if(!e.broker->authenticated()){
                    if(e.authenticateAttempted){e.view.state="authentication_required";e.view.stage="계좌 관리에서 ‘연결·재인증’을 눌러 주세요.";e.queued=false;continue;}
                    e.authenticateAttempted=true;e.view.state="authenticating";e.view.active=true;e.broker->authenticate();
                }else{e.choices=e.broker->accountChoices();e.view.state="selecting";e.view.active=true;}
            }else{e.view.state="reading";e.view.active=true;if(!beginAssets(e)){e.view.active=false;e.view.state="failed";e.view.stage=e.status();e.queued=false;}}
            if(e.view.active){++active;credentialActive.insert(e.view.account.credentialRef);}
        }
        for(auto& [id,p]:entries)if(!p->sharedWith.isEmpty()){auto* source=entry(p->sharedWith);if(source){const auto account=p->view.account;p->view=source->view;p->view.account=account;p->view.shared=true;}}
        syncBalanceStream();
        for(const auto& [id,e]:entries){
            const auto& view=e->view;
            const bool mayReconnect=!view.shared&&(view.state=="failed"||(e->broker&&!e->broker->authenticated()));
            PollStatus next{view.state,view.stage,view.accountScope,view.storageStatus,view.balanceStreamStatus,
                            view.receivedAtMs,view.elapsedMs/1000,view.active,mayReconnect&&q->canReconnectConnection(id)};
            if(next!=e->lastPollStatus){
                e->lastPollStatus=std::move(next);
                changed=true;
            }
        }
        if(changed)emit q->changed();
    }
};
PortfolioController::PortfolioController(Vault& v,QObject* parent,Factories factories):QObject(parent),d_(std::make_unique<Private>(this,v,std::move(factories))){
    d_->earn=std::make_unique<BinanceEarnCollector>(v,d_->history,d_->factories.earn?d_->factories.earn():nullptr);
    connect(d_->earn.get(),&BinanceEarnCollector::started,this,[this](const QString& scope){d_->activeEarnScope=scope;d_->earnScopeAttempts[scope]=d_->now();d_->earnAuthenticationBlocked.remove(scope);});
    connect(d_->earn.get(),&BinanceEarnCollector::finished,this,[this](bool){
        d_->lastEarnAttemptMs=d_->now()-60000;
        if(!d_->activeEarnScope.isEmpty()&&d_->earn->errorCode()=="AUTHENTICATION")d_->earnAuthenticationBlocked.insert(d_->activeEarnScope);
        d_->activeEarnScope.clear();emit changed();
    });
    d_->flow=std::make_unique<FundFlowCollector>(v,d_->history,d_->factories.flow?d_->factories.flow():nullptr);
    d_->flowReads=static_cast<bool>(d_->factories.flow)||!d_->factories.trading;
    connect(d_->flow.get(),&FundFlowCollector::finished,this,[this](bool ok){
        const auto scope=d_->flow->accountScope(),code=d_->flow->errorCode();
        // Key permission, sign-in and clock problems need the user; they are not retried every 10 minutes.
        if(!ok&&QStringList{"PERMISSION","AUTHENTICATION","CLOCK_SKEW","BINDING"}.contains(code)){d_->flowBlocked.insert(scope);d_->flowErrors[scope]=d_->flow->status();}
        else if(ok)d_->flowErrors.remove(scope);
        d_->lastFlowCheckMs=d_->now()-15000;d_->scheduleCoin();emit changed();
    });
    connect(d_->flow.get(),&FundFlowCollector::changed,this,&PortfolioController::changed);
    connect(d_->earn.get(),&BinanceEarnCollector::finished,this,[this](bool){d_->scheduleCoin();});
    d_->coinTimer.setSingleShot(true);connect(&d_->coinTimer,&QTimer::timeout,this,[this]{d_->evaluateCoin();});
    connect(&d_->history,&AssetsLedger::collectionWriteFinished,this,[this](quint64 receipt,bool,const QString&){
        if(receipt&&receipt==d_->baselineReceipt){d_->baselineReceipt=0;d_->scheduleCoin();}});
    connect(&d_->history,&AssetsLedger::changed,this,[this]{d_->scheduleCoin(2000);});
    connect(this,&PortfolioController::dayHistoryChanged,this,[this]{if(d_->windowReady)d_->scheduleCoin(1000);});
    connect(this,&PortfolioController::aboutToReset,this,[this]{d_->clearCoin();d_->clearDayHistory(QStringLiteral("연결 변경을 반영하고 있습니다."));emit dayHistoryChanged();});
    connect(&d_->history,&AssetsLedger::changed,this,[this]{d_->clearDayHistory(QStringLiteral("오늘 기록을 다시 확인하고 있습니다."));emit dayHistoryChanged();});
    d_->storageTimer.setSingleShot(true);connect(&d_->storageTimer,&QTimer::timeout,this,[this]{d_->flushStorage();});
    connect(&d_->latest,&AssetsLedger::writeFinished,this,[this](bool ok,const QString&){if(!d_->vault.isUnlocked())return;
        d_->latestSaveFailed=!ok;
        for(auto& [id,e]:d_->entries)if(e->view.receivedAtMs>0)e->view.storageStatus=ok?(d_->latestDirty?QStringLiteral("최신 조회 성공 · 마지막 자료 저장 대기"):QStringLiteral("마지막 자산 저장됨")):QStringLiteral("최신 조회 성공 · 마지막 자료 저장 실패");
        if(d_->latestDirty)d_->storageTimer.start(0);emit changed();});
    connect(&d_->history,&AssetsLedger::writeFinished,this,[this](bool ok,const QString&){if(!d_->vault.isUnlocked())return;
        d_->historySaveFailed=!ok;
        if(!ok){d_->jointHistoryAt=0;for(auto& [id,e]:d_->entries)e->view.storageStatus+=" · 과거 기록 저장 실패";}
        d_->savingHistoryScopes.clear();if(!d_->pendingHistory.isEmpty())d_->storageTimer.start(0);emit changed();});
    d_->priceTransport.reset(d_->factories.prices?d_->factories.prices():new NetworkTradingTransport);
    if(d_->priceTransport){
        d_->pricing=std::make_unique<PublicKrwPrices>(d_->priceTransport.get());
        connect(d_->pricing.get(),&PublicKrwPrices::changed,this,[this]{
            // A live tick for one asset can update the status after a failed REST lookup.
            // Keep that lookup's sanitized result until the next lookup or a vault reset.
            if(d_->pricing->report().failure==PublicPriceFailure::None)
                d_->lastPriceLookupFailure.clear();
            else if(d_->lastPriceLookupFailure.isEmpty())
                d_->lastPriceLookupFailure=d_->pricing->status();
            emit changed();
        });
        connect(d_->pricing.get(),&PublicKrwPrices::completed,this,[this](const QList<AssetQuote>& updates){
            if(!accountReadsAvailable(d_->vault))return;
            for(const auto& p:updates)d_->prices[d_->quoteKey(p)]=p;
            // The first usable pricing cycle must join the saved balances
            // promptly; otherwise a quick relock leaves an unpriced cache.
            // Subsequent market ticks keep the ten-second write debounce.
            if(d_->vault.isUnlocked()&&!d_->current.isEmpty()&&!updates.isEmpty()){
                d_->latestDirty=true;
                if(d_->initialPriceCheckpoint){d_->initialPriceBatch=true;d_->storageTimer.start(200);}
                else if(!d_->storageTimer.isActive())d_->storageTimer.start(10000);
            }
            if(d_->initialPriceBatch&&!d_->pricing->busy())d_->initialPriceCheckpoint=false;
            emit assetsChanged();emit changed();
        });
    }
    d_->poll.setInterval(250);connect(&d_->poll,&QTimer::timeout,this,[this]{d_->drive();});d_->automatic.setInterval(1000);
    connect(&d_->automatic,&QTimer::timeout,this,[this]{d_->queueAutomaticReads();});
    connect(&v,&Vault::aboutToLock,this,[this]{d_->clear();});
    connect(&v,&Vault::stateChanged,this,[this]{QTimer::singleShot(0,this,[this]{d_->synchronize();});});
    connect(&v,&Vault::accountsChanged,this,[this]{
        // Drop availability synchronously, before the queued rebuild can expose
        // the old connection selection. Only the date of a detected change is retained across lock.
        if(d_->vault.isUnlocked()&&!d_->comparisonBindings.isEmpty()&&d_->comparisonBindings!=d_->connectionBindings())
            d_->connectionChangeDay=portfolioHistoryDayStart(QDateTime::currentMSecsSinceEpoch());
        d_->comparisonResetPending=true;d_->manifest="__accounts_changed__";
        emit aboutToReset();emit changed();
        QTimer::singleShot(0,this,[this]{d_->synchronize();});
    });
}
PortfolioController::~PortfolioController(){
    // Private members die before QObject disconnects receiver connections. The
    // ledger's destructor emits changed(), so detach it while cache state lives.
    disconnect(&d_->history,nullptr,this,nullptr);
    disconnect(this,&PortfolioController::aboutToReset,this,nullptr);
    d_->clearDayHistory();
}
void PortfolioController::setAutomaticRefresh(bool enabled){d_->enabled=enabled;if(!enabled){d_->automatic.stop();d_->clearBalanceStream();if(d_->earn)d_->earn->stop();if(d_->flow)d_->flow->stop();return;}d_->synchronize();d_->automatic.start();d_->drive();}
void PortfolioController::setBinanceBalancePilot(bool enabled){d_->balancePilot=enabled;d_->balanceSuppressed=false;d_->clearBalanceStream();if(enabled)d_->syncBalanceStream();}
void PortfolioController::refreshAll(){if(!accountReadsAvailable(d_->vault))return;d_->synchronize();d_->balanceSuppressed=false;d_->clearBalanceStream();for(auto& [id,e]:d_->entries)if(e->view.state!="unsupported"&&!e->view.active&&!e->suspended){e->queued=true;e->automaticQueued=false;}d_->drive();}
void PortfolioController::refreshConnection(const QString& id){if(auto* e=d_->entry(id);e&&e->view.state!="unsupported"&&!e->view.active){
    if(e->view.account.provider=="binance")d_->balanceSuppressed=false;
    if(id==d_->balanceConnection){d_->balanceSuppressed=false;d_->clearBalanceStream();}
    e->queued=true;e->automaticQueued=false;d_->drive();}}
bool PortfolioController::canReconnectConnection(const QString& id) const {
    const auto* e=d_->entry(id);
    if(!d_->enabled||!accountReadsAvailable(d_->vault)||!e||e->view.active||e->suspended||e->busy())return false;
    for(const auto& [otherId,other]:d_->entries)
        if(other.get()!=e&&other->view.account.credentialRef==e->view.account.credentialRef&&
            (other->view.active||other->suspended||other->busy()))return false;
    if(e->broker)return (!e->broker->authenticated()||e->view.state=="failed")&&e->broker->canAuthenticate();
    if(e->view.state!="failed")return false;
    return e->okx?e->okx->canReconnectAssets():e->hyperliquid?e->hyperliquid->canReconnectAssets():e->spot?e->spot->canReconnectAssets():e->futures?e->futures->canReconnectAssets():false;
}
bool PortfolioController::reconnectConnection(const QString& id) {
    if(!canReconnectConnection(id))return false;
    if(id==d_->balanceConnection){d_->balanceSuppressed=false;d_->clearBalanceStream();}
    auto* e=d_->entry(id);e->queued=false;e->stopRequested=false;e->choices.clear();e->cycleScopes.clear();e->age.start();e->view.active=true;
    if(e->broker){e->authenticateAttempted=true;e->view.state="authenticating";e->broker->authenticate();}
    else {e->view.state="reading";const bool started=e->okx?e->okx->reconnectAssets():e->hyperliquid?e->hyperliquid->refreshAssets():e->spot?e->spot->reconnectAssets():e->futures->reconnectAssets();
        if(!started){e->view.active=false;e->view.state="failed";emit changed();return false;}}
    emit changed();return true;
}
void PortfolioController::stopReads(){
    if(d_->earn)d_->earn->stop();if(d_->flow)d_->flow->stop();
    d_->balanceSuppressed=true;d_->clearBalanceStream();
    if(d_->pricing)d_->pricing->clear();
    for(auto& [id,e]:d_->entries){
        e->queued=false;if(!e->view.active)continue;bool stopped=true;
        if(e->spot)stopped=e->spot->interruptRead();if(e->broker)stopped=e->broker->interruptRead();
        if(e->futures)stopped=e->futures->interruptRead();if(e->wallet)e->wallet->stop();
        if(e->okx)stopped=e->okx->interruptRead();if(e->hyperliquid)stopped=e->hyperliquid->interruptRead();
        e->stopRequested=!stopped;
        if(stopped){e->view.active=false;e->view.state="stopped";e->view.stage="조회 중단";}
        else e->view.stage="현재 인증 응답 후 조회 중단";
    }emit changed();
}
void PortfolioController::suspend(const QString& id,bool suspended){
    auto* e=d_->entry(id);if(!e || e->view.state=="unsupported")return;e->suspended=suspended;
    if(id==d_->balanceConnection){d_->balanceSuppressed=suspended;d_->clearBalanceStream();}
    if(!suspended)d_->balanceSuppressed=false;
    if(suspended&&e->view.active){
        bool stopped=true;if(e->spot)stopped=e->spot->interruptRead();if(e->broker)stopped=e->broker->interruptRead();
        if(e->futures)stopped=e->futures->interruptRead();if(e->wallet)e->wallet->stop();
        if(e->okx)stopped=e->okx->interruptRead();if(e->hyperliquid)stopped=e->hyperliquid->interruptRead();
        if(stopped){e->view.active=false;e->queued=false;e->view.state="manual";e->view.stage="거래 화면에서 조회 중";}
        else e->stopRequested=true;
    }
    // Trading refresh remains confined to the open manual screen; it never enables order mode.
    if(e->spot)e->spot->setAutomaticRefresh(suspended);if(e->broker)e->broker->setAutomaticRefresh(suspended);
    if(!suspended&&!e->view.active){e->queued=true;e->view.state="not_read";}d_->drive(true);
}
QList<PortfolioConnection> PortfolioController::connections()const{QList<PortfolioConnection> out;if(d_->comparisonResetPending)return out;for(const auto& [id,e]:d_->entries)out.append(e->view);return out;}
QList<AssetSnapshot> PortfolioController::snapshots()const{return accountReadsAvailable(d_->vault)?d_->current.values():QList<AssetSnapshot>{};}
std::optional<WalletReadSummary> PortfolioController::completedWalletRead(const QString& id) const {
    if (d_->comparisonResetPending || !accountReadsAvailable(d_->vault)) return {};
    const auto* entry = d_->entry(id);
    if (!entry || entry->view.account.id != id || entry->view.active || entry->view.state != "complete") return {};
    return entry->walletRead;
}
QList<AssetQuote> PortfolioController::quotes()const{return accountReadsAvailable(d_->vault)?d_->prices.values():QList<AssetQuote>{};}
QList<AssetQuote> PortfolioController::quotesForPair(const QString& base, const QString& currency) const {
    if (!accountReadsAvailable(d_->vault)) return {};
    QList<AssetQuote> quotes;
    const QString prefix = base + ":" + currency + ":";
    const auto& prices = d_->prices;
    for (auto it = prices.lowerBound(prefix); it != prices.cend() && it.key().startsWith(prefix); ++it) {
        if (it->baseAsset == base && it->quoteAsset == currency) quotes.append(it.value());
    }
    return quotes;
}

QList<AssetQuote> PortfolioController::referenceQuotes()const{return accountReadsAvailable(d_->vault)&&d_->pricing?d_->pricing->referenceQuotes():QList<AssetQuote>{};}
PortfolioDayHistory PortfolioController::dayHistory(const QSet<QString>& excludedAssets, qint64 asOfMs) const {
    const qint64 at=asOfMs>0?asOfMs:QDateTime::currentMSecsSinceEpoch();
    if(const auto reason=d_->unavailableDayReason(at);!reason.isEmpty())return Private::unavailableDay(reason);
    AssetsLedger::HistoryWindow window;QString error;
    const auto midnight=portfolioHistoryDayStart(at);
    if(!d_->history.readWindow(portfolioHistoryDayStart(midnight-1),at,&window,&error))return Private::unavailableDay(error);
    const auto previous=takePreviousDay(window,midnight);
    const auto selection=d_->daySelection(excludedAssets);QList<AssetSnapshot> usedSnapshots;
    const auto value=d_->currentDayValue(selection,at,&usedSnapshots);
    auto result=portfolioDayHistory(window.snapshots,window.quotes,selection,value,at);
    result.closingValuation=value;result.closingSnapshots=usedSnapshots;
    if(!result.points.isEmpty())result.previousDay=previousDayChange(previousDayClose(previous,selection),value,usedSnapshots);
    if(result.comparison.available)result.contributions=portfolioContributions(result.baseline.valuation,result.baseline.policyVersion,
        result.baseline.usedSnapshots,value,ValuationPolicyVersion,usedSnapshots);
    else result.contributions.reason=result.comparison.reason;
    if(!result.comparison.available)result.blockedConnectionNames=d_->blockedConnectionNames();
    return result;
}
void PortfolioController::requestDayHistory(const QSet<QString>& excludedAssets,qint64 asOfMs){
    const qint64 at=asOfMs>0?asOfMs:QDateTime::currentMSecsSinceEpoch();const auto day=portfolioHistoryDayStart(at);
    if(const auto reason=d_->unavailableDayReason(at);!reason.isEmpty()){d_->clearDayHistory(reason);return;}
    if(d_->cachedDay!=day||(d_->windowReady&&at<d_->loadedThrough))d_->clearDayHistory();
    d_->cachedDay=day;d_->wantedAt=at;d_->wantedExclusions=excludedAssets;
    if(d_->dayBusy)return; // Coalesce callbacks onto the newest selection/as-of.
    if(!d_->windowReady||(d_->dayWindow.nextEligibleMs>0&&at>=d_->dayWindow.nextEligibleMs)){
        d_->dayBusy=true;d_->windowReady=d_->pastReady=false;d_->dayWindow={};d_->previousWindow={};d_->previousClose={};d_->pastResult={};d_->pastTailValues.clear();
        d_->dayResult=Private::unavailableDay(QStringLiteral("오늘 기록을 불러오는 중입니다."));d_->dayReadError.clear();
        const auto generation=++d_->dayGeneration;
        d_->history.readWindowAsync(portfolioHistoryDayStart(day-1),at,[this,generation,at,day](bool ok,AssetsLedger::HistoryWindow window,QString error){
            if(generation!=d_->dayGeneration||!accountReadsAvailable(d_->vault))return;
            d_->dayBusy=false;d_->windowReady=true;d_->loadedThrough=at;
            d_->previousWindow=takePreviousDay(window,day);d_->dayWindow=std::move(window);
            d_->dayReadError=ok?QString{}:(error.isEmpty()?QStringLiteral("오늘 기록을 읽지 못했습니다."):error);
            // A future record may have become eligible while I/O was pending.
            const auto wanted=d_->wantedAt;const auto excluded=d_->wantedExclusions;
            requestDayHistory(excluded,wanted);emit dayHistoryChanged();
        });
        return;
    }
    d_->dayResult=d_->composeDay(at);
}
PortfolioDayHistory PortfolioController::dayHistoryResult()const{
    if(!accountReadsAvailable(d_->vault))return Private::unavailableDay(QStringLiteral("금고를 열면 표시합니다."));
    return d_->dayResult;
}
bool PortfolioController::dayHistoryBusy()const{return accountReadsAvailable(d_->vault)&&d_->dayBusy;}
QList<ProviderReconciliationResult> PortfolioController::providerReconciliations(qint64 asOfMs) const {
    QList<ProviderReconciliationResult> results;
    if (!accountReadsAvailable(d_->vault) || d_->synchronizing) return results;
    const auto at = asOfMs > 0 ? asOfMs : QDateTime::currentMSecsSinceEpoch();
    for (const auto& [id, entry] : d_->entries) {
        if (!entry->contributesToPortfolio()) continue;
        auto official = d_->providerTotals.value(id, unsupportedProviderTotal(entry->view.account));
        if (entry->view.account.provider == "binance" && entry->view.account.market == "usdm_futures" &&
            !d_->providerTotals.contains(id)) {
            official.availability = ProviderTotalAvailability::Unavailable;
            official.reason = QStringLiteral("공식 선물 총액을 아직 조회하지 못했습니다.");
        }
        const auto state = entry->view.state;
        if (official.availability != ProviderTotalAvailability::Unsupported && state != "complete") {
            official.availability = ProviderTotalAvailability::Unavailable;
            official.reason = state == "failed" ? QStringLiteral("계좌 조회가 실패해 공식 총액을 다시 확인해야 합니다.")
                                               : QStringLiteral("계좌 조회가 완료된 뒤 공식 총액을 비교합니다.");
        }
        const auto snapshot = d_->current.constFind(official.accountScope);
        results.append(reconcileProviderTotal(official, snapshot == d_->current.cend() ? nullptr : &snapshot.value(),
                                              d_->prices.values(), at));
    }
    return results;
}
QString PortfolioController::pricingStatus()const{return d_->pricing?d_->pricing->status():QString{};}
QString PortfolioController::pricingDiagnosticStatus()const{
    return d_->lastPriceLookupFailure.isEmpty()?pricingStatus():d_->lastPriceLookupFailure;
}
QString PortfolioController::pricingDetails()const{return d_->pricing?d_->pricing->details():QString{};}
bool PortfolioController::pricingBusy()const{return d_->pricing&&d_->pricing->busy();}
bool PortfolioController::pricingPending()const{return d_->pricing&&(d_->pricing->busy()||d_->pricing->report().streamPending>0);}
void PortfolioController::refreshPrices(){if(d_->pricing&&accountReadsAvailable(d_->vault))d_->pricing->refresh(d_->current.values());}
void PortfolioController::setPublicPrices(const QList<AssetQuote>& prices,const QStringList& markets){if(d_->pricing)d_->pricing->setPublicQuotes(prices,markets);}
void PortfolioController::setPublicMarketCatalog(PublicMarketCatalog* catalog){if(d_->pricing)d_->pricing->setPublicMarketCatalog(catalog);}
void PortfolioController::setBinancePriceStream(PublicQuoteStream* stream){if(d_->pricing)d_->pricing->setBinanceStream(stream);}
QString PortfolioController::canonicalConnection(const QString& id)const{return d_->canonical(id);}
TradingController* PortfolioController::trading(const QString& id){
    auto* e=d_->entry(id);if(!e)return nullptr;
    // The existing manual spot screen is independent of the complete background equity read.
    // It is created only when requested; its spot-only capture never replaces unified equity.
    if(e->okx&&e->view.account.market=="spot"&&!e->spot){
        auto* transport=d_->factories.trading?d_->factories.trading(e->view.account):nullptr;
        e->spot=transport?std::make_unique<TradingController>(d_->vault,transport):std::make_unique<TradingController>(d_->vault);e->spot->setAutomaticRefresh(e->suspended);e->spot->selectAccount(e->view.account.id);
    }
    return e->spot.get();
}
BrokerTradingController* PortfolioController::broker(const QString& id){if(d_->comparisonResetPending)return nullptr;const auto* e=d_->entry(id);return e?e->broker.get():nullptr;}
FuturesTradingController* PortfolioController::futures(const QString& id){const auto* e=d_->entry(id);return e?e->futures.get():nullptr;}
AssetsLedger* PortfolioController::historyLedger(){return &d_->history;}
AssetsLedger* PortfolioController::latestLedger(){return &d_->latest;}
BinanceEarnCollector* PortfolioController::earnCollector(){return d_->earn.get();}
CoinPerformanceView PortfolioController::coinPerformance()const{return d_->vault.isUnlocked()&&!d_->synchronizing&&!d_->comparisonResetPending?d_->coinView:CoinPerformanceView{};}
FundFlowCollector* PortfolioController::flowCollector(){return d_->flow.get();}
void PortfolioController::refreshCoinFlows(){
    if(!accountReadsAvailable(d_->vault))return;
    d_->flowBlocked.clear();d_->flowErrors.clear();d_->flowQueue.clear();
    for(const auto& s:d_->current)if(CoinPerformance::inScope(s))d_->flowQueue.append(s.accountScope);
    d_->lastFlowCheckMs=d_->now()-15000;d_->queueAutomaticReads();emit changed();
}
bool PortfolioController::confirmUpbitPockets(bool confirmed){
    if(!d_->flow||d_->flow->busy())return false;
    for(const auto& s:d_->current)if(s.provider=="upbit"&&CoinPerformance::inScope(s)&&d_->flow->setUpbitConfirmation(s.accountScope,confirmed))return true;
    return false;
}
PortfolioController::RecordCheckpoint PortfolioController::checkpointRecords(QString* message){
    if(message)message->clear();
    if(!d_->vault.isUnlocked()){if(message)*message="기록을 내보내려면 금고를 열어 주세요.";return RecordCheckpoint::Failed;}
    d_->storageTimer.stop();d_->flushStorage();
    if(d_->latest.isWriting()||d_->history.isWriting()||d_->latestDirty||!d_->pendingHistory.isEmpty()){
        if(message)*message="마지막 자산·주문 관찰 기록의 암호화 저장을 마무리하고 있습니다.";
        return RecordCheckpoint::Waiting;
    }
    if(d_->latestSaveFailed||d_->historySaveFailed){
        if(message)*message="기록 저장 실패가 있어 내보내기를 중단했습니다. 자산을 새로고침한 뒤 저장 상태를 확인하세요.";
        return RecordCheckpoint::Failed;
    }
    return RecordCheckpoint::Ready;
}
}
