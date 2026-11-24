#include "keefi/AssetsLedger.h"
#include "keefi/CoinPerformance.h"
#include "keefi/FundFlowCollector.h"
#include "keefi/FundFlowProtocol.h"
#include "keefi/FundFlowTracking.h"
#include "keefi/ResponseJournal.h"
#include "keefi/TradingTransport.h"
#include "keefi/Vault.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimeZone>
#include <QUrlQuery>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <sodium.h>
using namespace keefi;
namespace {
constexpr qint64 Now = 1793448000000LL; // 2026-10-31 21:00 KST.
constexpr qint64 Minute = 60000LL, Hour = 3600000LL, Day = 86400000LL;
const QString Uuid1 = "11111111-2222-4333-8444-555555555555", Uuid2 = "66666666-7777-4888-9999-aaaaaaaaaaaa";
CredentialSet upbitCredentials() {
    CredentialSet c; const QByteArray key = "SyntheticUpbitAccess123", secret = "SyntheticUpbitSecret123";
    c.emplace("access_key", SecureBuffer(key.constData(), std::size_t(key.size())));
    c.emplace("secret", SecureBuffer(secret.constData(), std::size_t(secret.size()))); return c;
}
CredentialSet binanceCredentials() {
    CredentialSet c; const QByteArray key = "SyntheticFlowKey123", secret = "SyntheticFlowSecret123";
    c.emplace("api_key", SecureBuffer(key.constData(), std::size_t(key.size())));
    c.emplace("secret", SecureBuffer(secret.constData(), std::size_t(secret.size()))); return c;
}
QJsonObject upbitRow(bool deposit, const QString& uuid, const QString& currency, const QString& amount, const QString& state,
                     qint64 created, qint64 done, const QString& fee = "0") {
    const auto iso = [](qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone(9 * 3600)).toString("yyyy-MM-ddTHH:mm:ss+09:00"); };
    return {{"type", deposit ? "deposit" : "withdraw"}, {"uuid", uuid}, {"currency", currency}, {"net_type", QJsonValue::Null},
        {"txid", "synthetic-tx"}, {"state", state}, {"created_at", iso(created)}, {"done_at", done ? QJsonValue(iso(done)) : QJsonValue::Null},
        {"amount", amount}, {"fee", fee}, {"transaction_type", "default"}};
}
HttpResult array(const QJsonArray& rows) { HttpResult h; h.status = 200; h.body = QJsonDocument(rows).toJson(QJsonDocument::Compact); return h; }
HttpResult object(const QJsonObject& o, int status = 200) { HttpResult h; h.status = status; h.body = QJsonDocument(o).toJson(QJsonDocument::Compact); return h; }
FlowQuery binance(FlowSource s, const QString& type = {}) { FlowQuery q; q.source = s; q.fromMs = Now - Day; q.throughMs = Now; q.transferType = type; return q; }

// Synthetic valuation inputs: KRW cash only, so no price route is involved.
AssetSnapshot balance(const QString& scope, const QString& connection, const QString& binding, const QString& provider,
                      const QString& market, qint64 at, const QString& krw) {
    AssetSnapshot s; s.id = scope.left(4) + QString::number(at); s.accountScope = scope; s.provider = provider; s.environment = "live";
    s.market = market; s.timeMs = at; s.balanceComplete = s.reconciled = true; s.connectionId = connection; s.connectionBinding = binding;
    s.evidence = {"synthetic official", "https://example.invalid/balance", scope, 0, at, true};
    AssetComponent c; c.id = "cash"; c.asset = "KRW"; c.quantity = krw; c.kind = AssetComponentKind::Cash; c.evidence = s.evidence;
    s.components = {c}; return s;
}
const QString UpScope = QString(64, 'a'), UpBinding = QString(64, 'b'), SpotScope = QString(64, 'c'), SpotBinding = QString(64, 'e'),
    UsdmScope = QString(64, 'd'), UsdmBinding = QString(64, 'f');
FlowAccountState covered(const QString& scope, const QString& connection, const QString& binding, const QString& provider, const QString& market) {
    FlowAccountState s; s.accountScope = scope; s.connectionId = connection; s.binding = binding; s.provider = provider; s.market = market;
    s.revision = 1; s.trackingStartMs = Now - Day; if (provider == "upbit") s.upbitConfirmedAtMs = Now - Day;
    for (const auto source : FundFlowTracking::sources(s)) {
        const auto types = source == FlowSource::BinanceTransfers ? FundFlowTracking::transferTypes(s) : QStringList{QString{}};
        for (const auto& type : types) s.windows.append({source, type, Now - 9 * Day, Now, Now});
    }
    return s;
}
FlowObservation observed(FlowSource source, const QString& asset, const QString& amount, qint64 at, bool final = true, bool success = true,
                         const QString& fee = "0", const QString& type = {}, const QString& reference = {}) {
    FlowRecord r; r.source = source; r.id = QCryptographicHash::hash(QByteArray::number(at) + asset.toUtf8() + type.toUtf8(), QCryptographicHash::Sha256).toHex();
    r.asset = asset; r.amount = amount; r.fee = fee; r.state = success ? "DONE" : "WAITING"; r.type = type; r.reference = reference;
    r.createdMs = at - Minute; r.doneMs = success ? at : 0; r.receivedAtMs = Now; r.final = final; r.success = success;
    if (flowUpbit(source) && !final) r.pendingRef = Uuid1;
    if (!flowUpbit(source)) r.wallet = 0;
    return {r.id, 1, r, false};
}
EarnAccountState earnCovered() {
    EarnAccountState e; e.accountScope = SpotScope; e.binding = SpotBinding; e.connectionId = "bs"; e.trackingStartMs = Now - Day; e.revision = 1;
    for (const auto source : earnSources()) if (!earnPositions(source)) e.windows.append({source, Now - 31 * Day, Now, Now});
    return e;
}
// Upbit only: opening at 19:00 KST with 1,000,000 KRW, closing at 20:50 with `closing`.
CoinPerformanceInput upbitOnly(const QString& closing) {
    CoinPerformanceInput in; in.nowMs = Now;
    in.history = {balance(UpScope, "up", UpBinding, "upbit", "spot", Now - 2 * Hour, "1000000"),
                  balance(UpScope, "up", UpBinding, "upbit", "spot", Now - 10 * Minute, closing)};
    in.current = {in.history.last()};
    in.flows = {covered(UpScope, "up", UpBinding, "upbit", "spot")};
    return in;
}
FlowAccountState maximumFlowState(const QString& scope, const QString& connection) {
    auto state = covered(scope, connection, UpBinding, "upbit", "spot"); state.windows.clear();
    // Schema-valid boundary input, not a claim that normal collection creates
    // this many disjoint coverage windows.
    for (int i = 0; i < FundFlowTracking::MaxWindows; ++i) {
        const auto start = Now - 7 * Day + qint64(i) * 2 * Minute;
        state.windows.append({i % 2 ? FlowSource::UpbitDeposits : FlowSource::UpbitWithdrawals, {}, start, start + Minute, Now});
    }
    for (int i = 0; i < FundFlowTracking::MaxObservations; ++i) {
        auto row = observed(FlowSource::UpbitDeposits, "KRW", "1", Now - 3 * Day + i);
        row.record.state = "ACCEPTED"; state.observations.append(row);
    }
    return state;
}
const QString Password = "Synthetic Flow Password! 2026";
QString bindingOf(Vault& v, const AccountRecord& a) {
    QCryptographicHash h(QCryptographicHash::Sha256); h.addData(QByteArrayView("KEEFI/current-assets/credential-binding/v1"));
    for (const auto& part : {a.provider, a.environment, a.market, a.region}) { const auto b = part.toUtf8(); h.addData(QByteArray::number(b.size())); h.addData(":"); h.addData(b); }
    v.withCredentials(a.id, [&](const CredentialSet& c) { for (const auto& [name, value] : c) {
        h.addData(name.toUtf8()); h.addData(":"); h.addData(QByteArray::number(value.size())); h.addData(":");
        h.addData(QByteArrayView(reinterpret_cast<const char*>(value.data()), qsizetype(value.size()))); } });
    return QString::fromLatin1(h.result().toHex());
}
AssetSnapshot liveSnapshot(Vault& v, const QString& scope) {
    const auto a = v.accounts().first(); auto s = balance(scope, a.id, bindingOf(v, a), a.provider, a.market, QDateTime::currentMSecsSinceEpoch(), "1000");
    return s;
}
class FlowWire final : public TradingTransport {
public:
    QList<RequestSpec> requests; std::function<HttpResult(const RequestSpec&)> answer;
    using TradingTransport::TradingTransport;
    void abort() override {}
    void send(quint64 id, const RequestSpec& r) override {
        requests.append(r);
        HttpResult h;
        if (r.operation == Operation::Time) { h.status = 200; h.body = QJsonDocument(QJsonObject{{"serverTime", QString::number(QDateTime::currentMSecsSinceEpoch())}}).toJson(QJsonDocument::Compact); }
        else h = answer(r);
        QMetaObject::invokeMethod(this, [this, id, h] { emit finished(id, h); }, Qt::QueuedConnection);
    }
};
} // namespace

class FundFlowTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(sodium_init() >= 0); }

    void checkpointSearchFindsValidEndpointsBeyondThreeHundredCandidates_data() {
        QTest::addColumn<bool>("opening");
        QTest::newRow("opening") << true; QTest::newRow("closing") << false;
    }
    void checkpointSearchFindsValidEndpointsBeyondThreeHundredCandidates() {
        QFETCH(bool, opening); auto in = upbitOnly("1050000");
        const auto validOpening = in.history.first().timeMs, validClosing = in.history.last().timeMs;
        for (int i = 0; i < 301; ++i) {
            auto invalid = balance(UpScope, "up", UpBinding, "upbit", "spot",
                (opening ? validOpening - Hour : validClosing + Minute) + i, "999999");
            invalid.balanceComplete = false; in.history.append(invalid);
        }
        const auto view = CoinPerformance::evaluate(in);
        QVERIFY2(view.today.available, qPrintable(view.today.reason));
        QCOMPARE(view.today.fromMs, validOpening); QCOMPARE(view.today.toMs, validClosing);
        QCOMPARE(view.today.pnl, QString("50000"));
        QVERIFY2(view.sinceStart.available, qPrintable(view.sinceStart.reason));
        QCOMPARE(view.sinceStart.fromMs, validOpening); QCOMPARE(view.sinceStart.toMs, validClosing);
    }
    void maximumSnapshotAndQuoteInputsKeepValidEndpoints() {
        auto in = upbitOnly("1050000"); const auto opening = in.history.first().timeMs;
        for (int i = 0; i < AssetsLedger::MaxSnapshots - 2; ++i) {
            auto invalid = balance(UpScope, "up", UpBinding, "upbit", "spot", opening - Hour + i, "0");
            invalid.balanceComplete = false; in.history.append(invalid);
        }
        for (int i = 0; i < AssetsLedger::MaxQuotes; ++i) {
            AssetEvidence e{"synthetic public", "https://example.invalid/price", "public:upbit:KRW-USDT", opening - 10000 + i, opening - 10000 + i, true};
            in.quotes.append({"USDT", "KRW", "1400", e});
        }
        QCOMPARE(in.history.size(), AssetsLedger::MaxSnapshots); QCOMPARE(in.quotes.size(), AssetsLedger::MaxQuotes);
        QElapsedTimer elapsed; elapsed.start(); const auto view = CoinPerformance::evaluate(in);
        qInfo().noquote() << QString("scale-checkpoints snapshots=%1 quotes=%2 evaluate_elapsed_ms=%3")
            .arg(in.history.size()).arg(in.quotes.size()).arg(elapsed.elapsed());
        QVERIFY2(view.today.available, qPrintable(view.today.reason));
        QCOMPARE(view.today.fromMs, opening); QCOMPARE(view.today.pnl, QString("50000"));
    }
    void maximumPairedTransfersKeepExactPnl() {
        CoinPerformanceInput in; in.nowMs = Now;
        for (const auto at : {Now - 2 * Hour, Now - 10 * Minute}) {
            in.history.append(balance(SpotScope, "bs", SpotBinding, "binance", "spot", at, "500000"));
            in.history.append(balance(UsdmScope, "bu", UsdmBinding, "binance", "usdm_futures", at, at == Now - 2 * Hour ? "0" : "20000"));
        }
        in.current = {in.history[2], in.history[3]}; in.earn = {earnCovered()};
        in.flows = {covered(SpotScope, "bs", SpotBinding, "binance", "spot"), covered(UsdmScope, "bu", UsdmBinding, "binance", "usdm_futures")};
        for (int i = 0; i < FundFlowTracking::MaxObservations; ++i) {
            const auto ref = QString::fromLatin1(QCryptographicHash::hash(QByteArray::number(i), QCryptographicHash::Sha256).toHex());
            const auto row = observed(FlowSource::BinanceTransfers, "USDT", "1", Now - Hour + i, true, true, "0", "MAIN_UMFUTURE", ref);
            in.flows[0].observations.append(row); in.flows[1].observations.append(row);
        }
        QElapsedTimer elapsed; elapsed.start(); const auto view = CoinPerformance::evaluate(in);
        qInfo().noquote() << QString("scale-transfers observations_per_account=%1 evaluate_elapsed_ms=%2")
            .arg(FundFlowTracking::MaxObservations).arg(elapsed.elapsed());
        QVERIFY2(view.today.available, qPrintable(view.today.reason));
        QVERIFY2(view.sinceStart.available, qPrintable(view.sinceStart.reason));
        QCOMPARE(view.today.pnl, QString("20000")); QCOMPARE(view.lines.size(), 2 * FundFlowTracking::MaxObservations);
    }
    void maximumFlowCountsRoundTripAndOversizedAggregatePreservesLedger() {
        auto state = maximumFlowState(UpScope, "synthetic-max-first"); FlowAccountState verified;
        QVERIFY(FundFlowTracking::decode(FundFlowTracking::encode(state), &verified)); QVERIFY(verified == state);
        QCOMPARE(state.observations.size(), FundFlowTracking::MaxObservations); QCOMPARE(state.windows.size(), FundFlowTracking::MaxWindows);
        auto tooMany = state; tooMany.observations.append(observed(FlowSource::UpbitDeposits, "KRW", "1", Now - Hour));
        QVERIFY(!FundFlowTracking::decode(FundFlowTracking::encode(tooMany), &verified));
        tooMany = state; tooMany.windows.append({FlowSource::UpbitDeposits, {}, Now - Minute, Now, Now});
        QVERIFY(!FundFlowTracking::decode(FundFlowTracking::encode(tooMany), &verified));
        QTemporaryDir dir; Vault vault(dir.filePath("synthetic-scale-vault")); QVERIFY(vault.create(Password));
        AssetsLedger ledger(&vault, dir.filePath("assets.keefi")); QVERIFY(ledger.open(true));
        const auto payloadBytes = QJsonDocument(FundFlowTracking::encode(state)).toJson(QJsonDocument::Compact).size();
        QVERIFY(payloadBytes < AssetsLedger::MaxFileBytes);
        QSignalSpy saved(&ledger, &AssetsLedger::collectionWriteFinished); QString error;
        QElapsedTimer persistence; persistence.start();
        QCOMPARE(ledger.saveFlowState(state, 0, 520, &error), AssetsLedger::CollectionWrite::Pending);
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 20000); QVERIFY2(saved[0][1].toBool(), qPrintable(saved[0][2].toString()));
        const auto saveMs = persistence.elapsed(); ledger.lock(); QVERIFY2(ledger.open(false, &error), qPrintable(error));
        QElapsedTimer decode; decode.start(); const auto stored = ledger.coinInputs().flows; const auto decodeMs = decode.elapsed();
        QCOMPARE(stored.size(), 1); QVERIFY(stored.first() == state);
        const auto bytes = [&] { QFile file(ledger.filePath()); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; };
        const auto before = bytes(); QVERIFY(!before.isEmpty()); QVERIFY(before.size() <= AssetsLedger::MaxFileBytes);
        auto second = state; second.accountScope = SpotScope; second.connectionId = "synthetic-max-second";
        const auto aggregateBytes = QJsonDocument(QJsonArray{FundFlowTracking::encode(state), FundFlowTracking::encode(second)}).toJson(QJsonDocument::Compact).size();
        QVERIFY(aggregateBytes > AssetsLedger::MaxFileBytes);
        QElapsedTimer rejection; rejection.start();
        QCOMPARE(ledger.saveFlowState(second, 0, 521, &error), AssetsLedger::CollectionWrite::Failed);
        const auto rejectionMs = rejection.elapsed();
        QVERIFY2(error.contains("크기 상한"), qPrintable(error)); QVERIFY(!ledger.isWriting());
        QCOMPARE(saved.size(), 1); QCOMPARE(bytes(), before);
        const auto after = ledger.coinInputs().flows; QCOMPARE(after.size(), 1); QVERIFY(after.first() == state);
        qInfo().noquote() << QString("scale-ledger observations=%1 windows=%2 payload_bytes=%3 cipher_bytes=%4 aggregate_bytes=%5 save_receipt_elapsed_ms=%6 coin_inputs_decode_elapsed_ms=%7 oversize_rejection_elapsed_ms=%8")
            .arg(state.observations.size()).arg(state.windows.size()).arg(payloadBytes).arg(before.size()).arg(aggregateBytes)
            .arg(saveMs).arg(decodeMs).arg(rejectionMs);
    }

    void readOnlyAuthorityIsExact() {
        const auto upbit = FundFlowProtocol::request({FlowSource::UpbitDeposits}, upbitCredentials(), Now);
        QVERIFY(allowedFundFlowRead(upbit)); QVERIFY(NetworkTradingTransport::allowed(upbit));
        QCOMPARE(upbit.url.query(), QString("limit=100&order_by=desc&page=1"));
        FlowQuery recheck; recheck.source = FlowSource::UpbitWithdrawals; recheck.uuids = {Uuid1, Uuid2};
        QVERIFY(NetworkTradingTransport::allowed(FundFlowProtocol::request(recheck, upbitCredentials(), Now)));
        for (const auto s : {FlowSource::BinanceDeposits, FlowSource::BinanceWithdrawals}) QVERIFY(NetworkTradingTransport::allowed(FundFlowProtocol::request(binance(s), binanceCredentials(), Now)));
        for (const auto& type : flowSpotTransferTypes() + flowUsdmTransferTypes()) {
            auto q = binance(FlowSource::BinanceTransfers, type); q.fromMs = Now - 6 * Day;
            QVERIFY2(NetworkTradingTransport::allowed(FundFlowProtocol::request(q, binanceCredentials(), Now)), qPrintable(type));
        }
        auto tampered = upbit; tampered.method = "POST"; QVERIFY(!NetworkTradingTransport::allowed(tampered));
        tampered = upbit; tampered.url.setPath("/v1/withdraws/coin"); QVERIFY(!NetworkTradingTransport::allowed(tampered));
        tampered = upbit; tampered.url.setQuery("limit=100&order_by=desc&page=1&currency=KRW"); QVERIFY(!NetworkTradingTransport::allowed(tampered));
        auto wide = binance(FlowSource::BinanceDeposits); wide.fromMs = Now - 31 * Day; QVERIFY(FundFlowProtocol::request(wide, binanceCredentials(), Now).url.isEmpty());
        auto future = binance(FlowSource::BinanceDeposits); future.throughMs = Now + 1; QVERIFY(FundFlowProtocol::request(future, binanceCredentials(), Now).url.isEmpty());
        auto other = binance(FlowSource::BinanceTransfers, "MARGIN_FUNDING"); QVERIFY(FundFlowProtocol::request(other, binanceCredentials(), Now).url.isEmpty());
        auto signedReq = FundFlowProtocol::request(binance(FlowSource::BinanceDeposits), binanceCredentials(), Now);
        signedReq.headers.insert("Authorization", "Bearer x"); QVERIFY(!NetworkTradingTransport::allowed(signedReq));
    }
    void upbitRowsKeepStateAndRejectAmbiguousTime() {
        const auto page = FundFlowProtocol::parsePage({FlowSource::UpbitDeposits}, array({
            upbitRow(true, Uuid1, "KRW", "100000.0", "ACCEPTED", Now - Hour, Now - Hour + 10000),
            upbitRow(true, Uuid2, "BTC", "0.1", "PROCESSING", Now - Minute, 0)}), Now);
        QVERIFY(page.ok); QCOMPARE(page.rows.size(), 2);
        QVERIFY(page.rows[0].final && page.rows[0].success); QCOMPARE(page.rows[0].amount, QString("100000")); QCOMPARE(page.rows[0].doneMs, Now - Hour + 10000);
        QVERIFY(!page.rows[1].final); QCOMPARE(page.rows[1].pendingRef, Uuid2);
        auto noOffset = upbitRow(true, Uuid1, "KRW", "1", "ACCEPTED", Now - Hour, Now - Hour); noOffset["created_at"] = "2026-10-31T19:00:00";
        QCOMPARE(FundFlowProtocol::parsePage({FlowSource::UpbitDeposits}, array({noOffset}), Now).errorCode, QString("ROW_INVALID"));
        auto acceptedWithoutDone = upbitRow(true, Uuid1, "KRW", "1", "ACCEPTED", Now - Hour, 0);
        const auto waiting = FundFlowProtocol::parsePage({FlowSource::UpbitDeposits}, array({acceptedWithoutDone}), Now);
        QVERIFY(waiting.ok); QVERIFY(!waiting.rows[0].final);
        QCOMPARE(FundFlowProtocol::parsePage({FlowSource::UpbitWithdrawals}, array({upbitRow(true, Uuid1, "KRW", "1", "DONE", Now - Hour, Now - Hour)}), Now).errorCode, QString("ROW_INVALID"));
        HttpResult denied = object({{"error", QJsonObject{{"name", "out_of_scope"}, {"message", "synthetic"}}}}, 401);
        QCOMPARE(FundFlowProtocol::parsePage({FlowSource::UpbitDeposits}, denied, Now).errorCode, QString("PERMISSION"));
        auto duplicate = array({}); duplicate.body = "[{\"type\":\"deposit\",\"type\":\"withdraw\"}]";
        QCOMPARE(FundFlowProtocol::parsePage({FlowSource::UpbitDeposits}, duplicate, Now).errorCode, QString("INVALID_JSON"));
    }
    void binanceRowsMapStatusWalletAndTime() {
        const auto deposits = FundFlowProtocol::parsePage(binance(FlowSource::BinanceDeposits), array({
            QJsonObject{{"id", "9007199254740993"}, {"amount", "10"}, {"coin", "USDT"}, {"status", 1}, {"insertTime", QString::number(Now - Hour)},
                {"completeTime", QString::number(Now - Hour + 5000)}, {"transferType", 0}, {"walletType", 1}, {"txId", "x"}},
            QJsonObject{{"id", "2"}, {"amount", "1"}, {"coin", "BTC"}, {"status", 6}, {"insertTime", QString::number(Now - Hour)}, {"transferType", 1}, {"walletType", 0}}}), Now);
        QVERIFY(deposits.ok); QCOMPARE(deposits.rows[0].id, QString("9007199254740993")); QVERIFY(deposits.rows[0].success);
        QCOMPARE(deposits.rows[0].wallet, 1); QCOMPARE(deposits.rows[0].doneMs, Now - Hour + 5000);
        QVERIFY(!deposits.rows[1].final); QVERIFY(deposits.rows[1].internal);
        const auto withdrawals = FundFlowProtocol::parsePage(binance(FlowSource::BinanceWithdrawals), array({
            QJsonObject{{"id", "w1"}, {"amount", "8.91"}, {"transactionFee", "0.004"}, {"coin", "USDT"}, {"status", 6}, {"applyTime", "2026-10-31 11:00:00"},
                {"completeTime", "2026-10-31 11:05:00"}, {"transferType", 0}, {"walletType", 0}}}), Now);
        QVERIFY(withdrawals.ok); QCOMPARE(withdrawals.rows[0].fee, QString("0.004"));
        QCOMPARE(withdrawals.rows[0].doneMs, QDateTime(QDate(2026, 10, 31), QTime(11, 5), QTimeZone::UTC).toMSecsSinceEpoch());
        const auto none = FundFlowProtocol::parsePage(binance(FlowSource::BinanceTransfers, "MAIN_UMFUTURE"), object({{"total", 0}}), Now);
        QVERIFY(none.ok); QCOMPARE(none.total, 0);
        const auto transfers = FundFlowProtocol::parsePage(binance(FlowSource::BinanceTransfers, "MAIN_UMFUTURE"), object({{"total", 1}, {"rows", QJsonArray{
            QJsonObject{{"asset", "USDT"}, {"amount", "1"}, {"type", "MAIN_UMFUTURE"}, {"status", "CONFIRMED"}, {"tranId", 11415955596LL}, {"timestamp", QString::number(Now - Hour)}}}}}), Now);
        QVERIFY(transfers.ok); QVERIFY(transfers.rows[0].success); QCOMPARE(transfers.rows[0].doneMs, Now - Hour);
        const auto wrongType = FundFlowProtocol::parsePage(binance(FlowSource::BinanceTransfers, "UMFUTURE_MAIN"), object({{"total", 1}, {"rows", QJsonArray{
            QJsonObject{{"asset", "USDT"}, {"amount", "1"}, {"type", "MAIN_UMFUTURE"}, {"status", "CONFIRMED"}, {"tranId", 1}, {"timestamp", QString::number(Now - Hour)}}}}}), Now);
        QCOMPARE(wrongType.errorCode, QString("ROW_INVALID"));
    }
    void mergeSealsIdsAndKeepsChangesVisible() {
        auto base = covered(UpScope, "up", UpBinding, "upbit", "spot"); base.observations.clear();
        const auto pending = FundFlowProtocol::parsePage({FlowSource::UpbitDeposits}, array({upbitRow(true, Uuid1, "BTC", "0.1", "PROCESSING", Now - Hour, 0)}), Now);
        FlowBatch b; b.rows = pending.rows; b.receivedAtMs = Now;
        SecureBuffer key(32); randombytes_buf(key.data(), key.size()); FlowAccountState next; QString error;
        QVERIFY2(FundFlowTracking::merge(base, b, key, &next, &error), qPrintable(error));
        QCOMPARE(next.observations.size(), 1); QVERIFY(next.observations[0].key != Uuid1); QCOMPARE(next.observations[0].record.pendingRef, Uuid1);
        const auto done = FundFlowProtocol::parsePage({FlowSource::UpbitDeposits}, array({upbitRow(true, Uuid1, "BTC", "0.1", "ACCEPTED", Now - Hour, Now - Minute)}), Now + 1000);
        FlowBatch b2; b2.rows = done.rows; b2.receivedAtMs = Now + 1000; FlowAccountState after;
        QVERIFY(FundFlowTracking::merge(next, b2, key, &after, &error));
        QCOMPARE(after.observations.size(), 1); QVERIFY(!after.observations[0].conflict); QVERIFY(after.observations[0].record.success);
        QVERIFY(after.observations[0].record.pendingRef.isEmpty());
        QVERIFY(!QJsonDocument(FundFlowTracking::encode(after)).toJson().contains(Uuid1.toUtf8()));
        const auto changed = FundFlowProtocol::parsePage({FlowSource::UpbitDeposits}, array({upbitRow(true, Uuid1, "BTC", "0.2", "ACCEPTED", Now - Hour, Now - Minute)}), Now + 2000);
        FlowBatch b3; b3.rows = changed.rows; b3.receivedAtMs = Now + 2000; FlowAccountState conflicted;
        QVERIFY(FundFlowTracking::merge(after, b3, key, &conflicted, &error)); QVERIFY(conflicted.observations[0].conflict);
        FlowAccountState decoded; QVERIFY(FundFlowTracking::decode(FundFlowTracking::encode(conflicted), &decoded)); QVERIFY(decoded == conflicted);
        auto extra = FundFlowTracking::encode(conflicted); extra["address"] = "synthetic"; QVERIFY(!FundFlowTracking::decode(extra, &decoded));
    }
    void planReadsFromCoverageWithOverlapAndRechecksPending() {
        FlowAccountState fresh; fresh.accountScope = SpotScope; fresh.connectionId = "bs"; fresh.binding = SpotBinding; fresh.provider = "binance";
        fresh.market = "spot"; fresh.trackingStartMs = Now;
        const auto first = FundFlowTracking::plan(fresh, Now);
        QMap<QString, qint64> starts; int transfers = 0;
        for (const auto& j : first) if (j.query.source == FlowSource::BinanceTransfers) {
            ++transfers; starts[j.query.transferType] = starts.contains(j.query.transferType) ? qMin(starts[j.query.transferType], j.query.fromMs) : j.query.fromMs;
            QVERIFY(j.query.throughMs - j.query.fromMs <= FundFlowProtocol::TransferWindowMs); QCOMPARE(j.coverThroughMs, qMin(j.query.throughMs, Now - FundFlowTracking::ListingLagMs));
        }
        QCOMPARE(transfers, flowSpotTransferTypes().size() * 2); // Seven days, end-inclusive, span two windows.
        for (const auto start : starts) QCOMPARE(start, Now - 7 * Day);
        auto upbit = covered(UpScope, "up", UpBinding, "upbit", "spot");
        upbit.observations = {observed(FlowSource::UpbitWithdrawals, "BTC", "1", Now - Hour, false, false)};
        const auto jobs = FundFlowTracking::plan(upbit, Now + Hour);
        QCOMPARE(jobs.size(), 3); QCOMPARE(jobs[0].stopAtMs, Now - Day); QCOMPARE(jobs[2].query.uuids, QStringList{Uuid1});
        auto stale = upbit; stale.observations[0].record.createdMs = Now - 8 * Day;
        QCOMPARE(FundFlowTracking::plan(stale, Now + Hour).size(), 2);
    }
    void quietPeriodGivesExactPnl() {
        const auto view = CoinPerformance::evaluate(upbitOnly("1050000"));
        QVERIFY(view.inScope); QVERIFY2(view.today.available, qPrintable(view.today.reasons.join(" | ")));
        QCOMPARE(view.today.pnl, QString("50000")); QCOMPARE(view.today.fromMs, Now - 2 * Hour); QCOMPARE(view.today.toMs, Now - 10 * Minute);
        QVERIFY(!view.proposedBaseline.isEmpty()); QCOMPARE(view.proposedBaseline.value("netAssets").toString(), QString("1000000"));
    }
    void krwDepositIsCapitalNotProfit() {
        auto in = upbitOnly("1350000");
        in.flows[0].observations = {observed(FlowSource::UpbitDeposits, "KRW", "300000", Now - Hour)};
        const auto view = CoinPerformance::evaluate(in);
        QVERIFY2(view.today.available, qPrintable(view.today.reasons.join(" | ")));
        QCOMPARE(view.today.pnl, QString("50000")); QCOMPARE(view.today.inflow, QString("300000"));
        in.flows[0].observations = {observed(FlowSource::UpbitWithdrawals, "KRW", "100000", Now - Hour)};
        in.history.last().components[0].quantity = "950000"; in.current = {in.history.last()};
        const auto out = CoinPerformance::evaluate(in);
        QVERIFY(out.today.available); QCOMPARE(out.today.pnl, QString("50000")); QCOMPARE(out.today.outflow, QString("100000"));
    }
    void blockingMovementsHoldTheNumber_data() {
        QTest::addColumn<int>("kind"); QTest::addColumn<QString>("reason");
        QTest::newRow("boundary") << 0 << "잔고 기록 시각 근처의 입출금이 있어 다음 기록을 기다리고 있어요";
        QTest::newRow("coin") << 1 << "코인 입출금·지갑 이동이 있어 이 기간은 아직 계산하지 않았어요";
        QTest::newRow("pending") << 2 << "처리 중인 입출금이 있어 계산을 기다리고 있어요";
        QTest::newRow("fee") << 3 << "수수료가 있는 원화 입출금은 아직 계산하지 않았어요";
        QTest::newRow("unconfirmed") << 4 << "업비트 포켓·스테이킹을 쓰지 않는지 확인해 주세요";
        QTest::newRow("conflict") << 5 << "거래소 기록 내용이 바뀐 내역이 있어 확인이 필요해요";
    }
    void blockingMovementsHoldTheNumber() {
        QFETCH(int, kind); QFETCH(QString, reason);
        auto in = upbitOnly("1050000"); auto& f = in.flows[0];
        if (kind == 0) f.observations = {observed(FlowSource::UpbitDeposits, "KRW", "1", Now - 10 * Minute - 30000)};
        if (kind == 1) f.observations = {observed(FlowSource::UpbitDeposits, "BTC", "0.01", Now - Hour)};
        if (kind == 2) f.observations = {observed(FlowSource::UpbitWithdrawals, "KRW", "1", Now - Hour, false, false)};
        if (kind == 3) f.observations = {observed(FlowSource::UpbitWithdrawals, "KRW", "100000", Now - Hour, true, true, "1000")};
        if (kind == 4) f.upbitConfirmedAtMs = 0;
        if (kind == 5) { f.observations = {observed(FlowSource::UpbitDeposits, "KRW", "1", Now - Hour)}; f.observations[0].conflict = true; }
        const auto view = CoinPerformance::evaluate(in);
        QVERIFY(!view.today.available); QVERIFY2(view.today.reasons.contains(reason), qPrintable(view.today.reasons.join(" | ")));
        QVERIFY(view.today.pnl.isEmpty());
        if (kind == 4) QCOMPARE(view.upbitScopesToConfirm, QStringList{UpScope});
    }
    void missingCoverageNeverCountsAsQuiet() {
        auto in = upbitOnly("1050000");
        for (auto& w : in.flows[0].windows) w.throughMs = Now - Hour; // Read only up to 20:00.
        auto view = CoinPerformance::evaluate(in);
        QVERIFY(!view.today.available); QVERIFY(view.today.pnl.isEmpty());
        in = upbitOnly("1050000"); in.flows.clear();
        view = CoinPerformance::evaluate(in);
        QVERIFY(!view.today.available); QCOMPARE(view.today.reason, QString("입출금 기록을 확인하고 있어요"));
        in = upbitOnly("1050000"); in.flowErrors.insert(UpScope, "업비트 키에 입금·출금 조회 권한이 없어요.");
        view = CoinPerformance::evaluate(in);
        QVERIFY(!view.today.available); QVERIFY(view.today.reasons.contains("업비트 키에 입금·출금 조회 권한이 없어요."));
    }
    void binanceNeedsEarnAndBothTransferLegs() {
        CoinPerformanceInput in; in.nowMs = Now;
        for (const auto at : {Now - 2 * Hour, Now - 10 * Minute}) {
            in.history.append(balance(SpotScope, "bs", SpotBinding, "binance", "spot", at, at == Now - 2 * Hour ? "500000" : "400000"));
            in.history.append(balance(UsdmScope, "bu", UsdmBinding, "binance", "usdm_futures", at, at == Now - 2 * Hour ? "0" : "120000"));
        }
        in.current = {in.history[2], in.history[3]};
        in.flows = {covered(SpotScope, "bs", SpotBinding, "binance", "spot"), covered(UsdmScope, "bu", UsdmBinding, "binance", "usdm_futures")};
        auto view = CoinPerformance::evaluate(in);
        QVERIFY(!view.today.available); QVERIFY(view.today.reasons.contains("바이낸스 Earn 기록을 한 번 조회해 주세요 (자산 기록 → Earn 원금·보상)"));
        in.earn = {earnCovered()};
        view = CoinPerformance::evaluate(in);
        QVERIFY2(view.today.available, qPrintable(view.today.reasons.join(" | "))); QCOMPARE(view.today.pnl, QString("20000"));
        const QString ref(64, '9');
        in.flows[0].observations = {observed(FlowSource::BinanceTransfers, "USDT", "100", Now - Hour, true, true, "0", "MAIN_UMFUTURE", ref)};
        view = CoinPerformance::evaluate(in);
        QVERIFY(!view.today.available); QVERIFY(view.today.reasons.contains("내 계좌 사이의 이동인지 확인하지 못한 내역이 있어요"));
        in.flows[1].observations = {observed(FlowSource::BinanceTransfers, "USDT", "100", Now - Hour, true, true, "0", "MAIN_UMFUTURE", ref)};
        view = CoinPerformance::evaluate(in);
        QVERIFY2(view.today.available, qPrintable(view.today.reasons.join(" | "))); QCOMPARE(view.today.pnl, QString("20000"));
        in.flows[1].observations.clear();
        in.flows[0].observations = {observed(FlowSource::BinanceTransfers, "USDT", "100", Now - Hour, true, true, "0", "MAIN_FUNDING", ref)};
        view = CoinPerformance::evaluate(in);
        QVERIFY(view.today.reasons.contains("코인 입출금·지갑 이동이 있어 이 기간은 아직 계산하지 않았어요"));
        in.flows[0].observations.clear();
        EarnRecord sub; sub.source = EarnSource::FlexibleSubscriptions; sub.id = QString(64, '7'); sub.asset = "USDT"; sub.amount = "10";
        sub.spotAmount = "10"; sub.fundingAmount = "0"; sub.status = "SUCCESS"; sub.complete = true; sub.timeMs = Now - Hour; sub.receivedAtMs = Now;
        in.earn[0].observations = {{sub.id, 1, sub, false}};
        view = CoinPerformance::evaluate(in);
        QVERIFY(view.today.reasons.contains("바이낸스 Earn 이동이 있어 이 기간은 계산하지 않았어요"));
    }
    void storedStartStaysFixedAndScopeChangeStartsAgain() {
        auto in = upbitOnly("1050000");
        const auto first = CoinPerformance::evaluate(in);
        in.baseline = first.proposedBaseline;
        in.history.prepend(balance(UpScope, "up", UpBinding, "upbit", "spot", Now - 3 * Hour, "999999"));
        auto view = CoinPerformance::evaluate(in);
        QVERIFY(view.proposedBaseline.isEmpty()); QCOMPARE(view.sinceStart.fromMs, Now - 2 * Hour);
        QVERIFY(view.sinceStart.available); QCOMPARE(view.sinceStart.pnl, QString("50000"));
        in.history.removeFirst(); in.history.removeFirst(); // The opening snapshot moved to an archive.
        view = CoinPerformance::evaluate(in);
        QCOMPARE(view.sinceStart.fromMs, Now - 2 * Hour); QVERIFY(view.proposedBaseline.isEmpty());
        for (auto& s : in.history) s.connectionBinding = QString(64, '0');
        in.current = {in.history.last()}; in.flows[0].binding = QString(64, '0');
        view = CoinPerformance::evaluate(in);
        QVERIFY(view.sinceStart.fromMs != Now - 2 * Hour || !view.sinceStart.available);
    }
    void ledgerStoresFlowsAndStartBehindSchemaThree() {
        QTemporaryDir dir; Vault v(dir.filePath("vault")); QVERIFY(v.create(Password));
        AccountRecord a; a.provider = "upbit"; a.environment = "live"; a.market = "spot"; a.alias = "synthetic"; a.currency = "KRW";
        QVERIFY(v.addAccount(a, {{"access_key", "SyntheticUpbitAccess123"}, {"secret", "SyntheticUpbitSecret123"}}, StorageMode::EncryptedVault));
        AssetsLedger ledger(&v, dir.filePath("assets.keefi")); QVERIFY(ledger.open(true));
        auto state = covered(UpScope, v.accounts().first().id, UpBinding, "upbit", "spot"); state.revision = 1; QString error;
        QSignalSpy saved(&ledger, &AssetsLedger::collectionWriteFinished);
        QCOMPARE(ledger.saveFlowState(state, 0, 501, &error), AssetsLedger::CollectionWrite::Pending); QTRY_COMPARE(saved.size(), 1); QVERIFY(saved[0][1].toBool());
        QCOMPARE(QDir(dir.path()).entryList({"assets.keefi.pre-schema3-*.keefi"}, QDir::Files).size(), 1);
        const auto baseline = CoinPerformance::evaluate(upbitOnly("1050000")).proposedBaseline;
        QCOMPARE(ledger.saveCoinBaseline(baseline, 502, &error), AssetsLedger::CollectionWrite::Pending); QTRY_COMPARE(saved.size(), 2);
        ledger.lock(); QVERIFY(ledger.open(false));
        const auto inputs = ledger.coinInputs(); QCOMPARE(inputs.flows.size(), 1); QVERIFY(inputs.flows.first() == state); QCOMPARE(inputs.baseline, baseline);
        QCOMPARE(ledger.saveFlowState(state, 0, 503, &error), AssetsLedger::CollectionWrite::Failed);
        auto stale = state; stale.binding = QString(64, '1'); stale.revision = 2;
        QCOMPARE(ledger.saveFlowState(stale, 1, 504, &error), AssetsLedger::CollectionWrite::Failed);
        QCOMPARE(ledger.saveCoinBaseline(QJsonObject{{"kind", "coin-baseline-v1"}}, 505, &error), AssetsLedger::CollectionWrite::Failed);
    }
    void closedCoinInputsReadWithoutTakingWriterLock() {
        QTemporaryDir dir;Vault v(dir.filePath("vault"));QVERIFY(v.create(Password));
        const auto in=upbitOnly("1050000");
        AssetsLedger writer(&v,dir.filePath("assets.keefi"));QVERIFY(writer.open(true));
        QVERIFY(writer.append(in.history,{},in.quotes));
        QSignalSpy saved(&writer,&AssetsLedger::collectionWriteFinished);
        QCOMPARE(writer.saveFlowState(in.flows.first(),0,601),AssetsLedger::CollectionWrite::Pending);
        QTRY_COMPARE(saved.size(),1);QVERIFY(saved[0][1].toBool());
        const auto baseline=CoinPerformance::evaluate(in).proposedBaseline;
        QCOMPARE(writer.saveCoinBaseline(baseline,602),AssetsLedger::CollectionWrite::Pending);
        QTRY_COMPARE(saved.size(),2);QVERIFY(saved[1][1].toBool());
        QFile beforeFile(writer.filePath());QVERIFY(beforeFile.open(QIODevice::ReadOnly));
        const auto before=beforeFile.readAll();beforeFile.close();
        AssetsLedger reader(&v,writer.filePath());QSignalSpy changed(&reader,&AssetsLedger::changed);
        const auto inputs=reader.coinInputs(); // Another object still owns the writer lock.
        QCOMPARE(inputs.snapshots.size(),in.history.size());
        QCOMPARE(inputs.flows.size(),1);QVERIFY(inputs.flows.first()==in.flows.first());QCOMPARE(inputs.baseline,baseline);
        QVERIFY(!reader.isOpen());QVERIFY(changed.isEmpty());
        writer.lock();
        QCOMPARE(reader.coinInputs().snapshots.size(),in.history.size());
        QVERIFY(!QFileInfo::exists(writer.filePath()+".lock"));
        QFile afterFile(writer.filePath());QVERIFY(afterFile.open(QIODevice::ReadOnly));QCOMPARE(afterFile.readAll(),before);
        QVERIFY(writer.open(false)); // Reading did not reserve the writer for later.
        v.lock();QVERIFY(reader.coinInputs().snapshots.isEmpty());
    }
    void journalOmitsDepositAndWithdrawalBodies() {
        ResponseJournal::setEnabled(true); ResponseJournal::clear();
        HttpResult h = array({upbitRow(true, Uuid1, "KRW", "1", "ACCEPTED", Now - Hour, Now - Hour)});
        ResponseJournal::recordTrading(Operation::FundFlowRead, "GET", QUrl("https://api.upbit.com/v1/deposits"), true, h);
        QVERIFY(ResponseJournal::entries().isEmpty());
        ResponseJournal::setEnabled(false); ResponseJournal::clear();
    }
    void collectorReadsUpbitStoresAndReportsMissingPermission() {
        QTemporaryDir dir; Vault v(dir.filePath("vault")); QVERIFY(v.create(Password));
        AccountRecord a; a.provider = "upbit"; a.environment = "live"; a.market = "spot"; a.alias = "synthetic"; a.currency = "KRW";
        QVERIFY(v.addAccount(a, {{"access_key", "SyntheticUpbitAccess123"}, {"secret", "SyntheticUpbitSecret123"}}, StorageMode::EncryptedVault));
        AssetsLedger ledger(&v, dir.filePath("assets.keefi")); QVERIFY(ledger.open(true));
        auto* wire = new FlowWire;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        wire->answer = [now](const RequestSpec& r) {
            if (r.url.path() == "/v1/deposits") return array({upbitRow(true, Uuid1, "KRW", "50000", "ACCEPTED", now - Hour, now - Hour + 5000),
                                                              upbitRow(true, Uuid2, "BTC", "0.1", "PROCESSING", now - Minute, 0)});
            return array({});
        };
        FundFlowCollector::TestHooks hooks; hooks.upbitPacingMs = 0; hooks.binancePacingMs = 0;
        FundFlowCollector collector(v, ledger, wire, hooks); wire->setParent(&collector);
        QSignalSpy finished(&collector, &FundFlowCollector::finished);
        QVERIFY(collector.start(liveSnapshot(v, UpScope))); QTRY_COMPARE(finished.size(), 1); QVERIFY2(finished[0][0].toBool(), qPrintable(collector.status()));
        for (const auto& r : wire->requests) QVERIFY(NetworkTradingTransport::allowed(r));
        const auto states = ledger.flowStates(); QCOMPARE(states.size(), 1); QCOMPARE(states[0].observations.size(), 2); QCOMPARE(states[0].windows.size(), 2);
        QVERIFY(FundFlowTracking::covers(states[0], FlowSource::UpbitDeposits, {}, now - 7 * Day + Minute, now - 2 * Minute));
        QFile cipher(ledger.filePath()); QVERIFY(cipher.open(QIODevice::ReadOnly)); const auto raw = cipher.readAll();
        QVERIFY(!raw.contains(Uuid1.toUtf8())); QVERIFY(!raw.contains("50000"));
        // Second run rechecks the pending uuid; a key without the inquiry scope is reported as such.
        wire->answer = [](const RequestSpec&) { return object({{"error", QJsonObject{{"name", "out_of_scope"}, {"message", "synthetic"}}}}, 401); };
        finished.clear(); QVERIFY(collector.start(liveSnapshot(v, UpScope))); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].toBool()); QCOMPARE(collector.errorCode(), QString("PERMISSION")); QVERIFY(collector.status().contains("입금조회"));
        QVERIFY(ledger.flowStates().first() == states.first());
        finished.clear(); QVERIFY(collector.setUpbitConfirmation(UpScope, true)); QTRY_COMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool());
        QVERIFY(ledger.flowStates().first().upbitConfirmedAtMs > 0);
    }
    void collectorReadsEveryBinanceSpotSource() {
        QTemporaryDir dir; Vault v(dir.filePath("vault")); QVERIFY(v.create(Password));
        AccountRecord a; a.provider = "binance"; a.environment = "live"; a.market = "spot"; a.alias = "synthetic"; a.currency = "USDT";
        QVERIFY(v.addAccount(a, {{"api_key", "SyntheticFlowKey123"}, {"secret", "SyntheticFlowSecret123"}}, StorageMode::EncryptedVault));
        AssetsLedger ledger(&v, dir.filePath("assets.keefi")); QVERIFY(ledger.open(true));
        auto* wire = new FlowWire;
        wire->answer = [](const RequestSpec& r) { return r.url.path() == "/sapi/v1/asset/transfer" ? object({{"total", 0}}) : array({}); };
        FundFlowCollector::TestHooks hooks; hooks.upbitPacingMs = 0; hooks.binancePacingMs = 0;
        FundFlowCollector collector(v, ledger, wire, hooks); wire->setParent(&collector);
        QSignalSpy finished(&collector, &FundFlowCollector::finished);
        QVERIFY(collector.start(liveSnapshot(v, SpotScope))); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000); QVERIFY2(finished[0][0].toBool(), qPrintable(collector.status()));
        QSet<QString> types; for (const auto& r : wire->requests) { QVERIFY(NetworkTradingTransport::allowed(r)); if (r.url.path() == "/sapi/v1/asset/transfer") types.insert(QUrlQuery(r.url).queryItemValue("type")); }
        const auto expected = flowSpotTransferTypes(); QCOMPARE(types, QSet<QString>(expected.cbegin(), expected.cend()));
        QCOMPARE(ledger.flowStates().first().windows.size(), 2 + flowSpotTransferTypes().size());
    }
};
QTEST_MAIN(FundFlowTest)
#include "tst_fund_flows.moc"
