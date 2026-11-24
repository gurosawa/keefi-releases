#include "keefi/IdentityCompatibility.h"
#include "keefi/HyperliquidReadController.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/Vault.h"
#include "keefi/TradingTransport.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

namespace keefi {
namespace {
QString hash(const QString& text) {
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}
bool amount(const QString& value) {
    static const QRegularExpression pattern("\\A-?[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z");
    return pattern.match(value).hasMatch();
}
bool unsignedAmount(const QJsonValue& value) {
    return value.isString() && !value.toString().startsWith('-') && amount(value.toString());
}
QString number(const QJsonValue& value) { return value.isString() && amount(value.toString()) ? value.toString() : QString(); }
QString supportedCoverage() {
    return QStringLiteral("Hyperliquid 기본 USDC 선물(기본 DEX) · Standard 계좌 equity·포지션. "
        "미실현손익은 equity에 포함되어 별도 가산하지 않음. HIP-3·현물·통합계좌·Portfolio Margin·과거 이력 미포함.");
}
}

struct HyperliquidReadController::Private {
    HyperliquidReadController* q;
    Vault& vault;
    TradingTransport* transport;
    AccountRecord account;
    QString address, mode, message = "조회용 Phantom Ethereum 주소를 선택하세요.";
    QSet<QString> universe;
    AssetCapture latest;
    QList<PositionRecord> positions;
    AssetCapture pending;
    QList<PositionRecord> pendingPositions;
    bool active = false;
    int stage = 0;
    quint64 token = 0;
    QElapsedTimer clock;
    qint64 cooldownUntil = 0;
    qint64 readSourceTime = 0, lastSourceTime = 0;

    Private(HyperliquidReadController* owner, Vault& v, TradingTransport* t) : q(owner), vault(v), transport(t) { clock.start(); }
    void fail(const QString& reason) {
        active = false;
        universe.clear();
        pending = {}; pendingPositions.clear();
        address.clear();
        message = "조회 실패 · " + reason;
        if (latest.snapshot.timeMs > 0) message += " · 이전 확인 값 유지, 최신 잔고 아님";
        emit q->changed();
    }
    void send() {
        RequestSpec request;
        request.method = "POST";
        request.url = QUrl("https://api.hyperliquid.xyz/info");
        request.headers["Content-Type"] = "application/json";
        QJsonObject body;
        if (stage == 0) {
            request.operation = Operation::AccountConfig;
            body = {{"type", "userAbstraction"}, {"user", address}};
            message = "Hyperliquid 계좌 모드 확인 중";
        } else if (stage == 1) {
            request.operation = Operation::Rules;
            body = {{"type", "metaAndAssetCtxs"}};
            message = "Hyperliquid 기본 USDC 담보 범위 확인 중";
        } else if (stage == 2) {
            request.operation = Operation::Account;
            body = {{"type", "clearinghouseState"}, {"user", address}};
            message = "Hyperliquid 기본 USDC 선물 잔고·포지션 조회 중";
        } else {
            request.operation = Operation::FundingBalance;
            body = {{"type", "spotClearinghouseState"}, {"user", address}};
            message = "Hyperliquid 통합 계좌 공유 USDC 원본잔고 조회 중";
        }
        request.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        const auto current = ++token;
        emit q->changed();
        if (!active || current != token || !accountCredentialsAvailable(vault, account.id)) return;
        transport->send(current, request);
    }
    bool metadata(const QJsonDocument& document) {
        if (!document.isArray()) return false;
        const auto values = document.array();
        if (values.size() != 2 || !values[0].isObject() || !values[1].isArray()) return false;
        const auto meta = values[0].toObject();
        if (meta.value("collateralToken").toString() != "0" || !meta.value("universe").isArray()) return false;
        const auto rows = meta.value("universe").toArray();
        if (rows.isEmpty() || rows.size() > 10000 || rows.size() != values[1].toArray().size()) return false;
        static const QRegularExpression namePattern("\\A[A-Za-z0-9][A-Za-z0-9._-]{0,63}\\z");
        QSet<QString> names;
        for (const auto& value : rows) {
            const auto name = value.toObject().value("name").toString();
            if (!value.isObject() || !namePattern.match(name).hasMatch() || names.contains(name)) return false;
            names.insert(name);
        }
        universe = std::move(names);
        return true;
    }
    bool publish(const QJsonDocument& document) {
        if (!document.isObject()) return false;
        const auto raw = document.object();
        const auto summary = raw.value("marginSummary").toObject();
        const auto cross = raw.value("crossMarginSummary").toObject();
        for (const auto& row : {summary, cross}) {
            if (number(row.value("accountValue")).isEmpty() || number(row.value("totalRawUsd")).isEmpty() ||
                !unsignedAmount(row.value("totalMarginUsed")) || !unsignedAmount(row.value("totalNtlPos"))) return false;
        }
        if (!unsignedAmount(raw.value("withdrawable")) || !unsignedAmount(raw.value("crossMaintenanceMarginUsed")) ||
            !raw.value("assetPositions").isArray()) return false;
        const auto timeText = raw.value("time").toString();
        static const QRegularExpression integer("\\A[1-9][0-9]{0,15}\\z");
        bool validTime = false;
        const auto source = timeText.toLongLong(&validTime), received = QDateTime::currentMSecsSinceEpoch();
        if (!integer.match(timeText).hasMatch() || !validTime || received <= 0 || source > received + 2000 || received - source > 15000 ||
            (lastSourceTime > 0 && source < lastSourceTime)) return false;
        const auto rows = raw.value("assetPositions").toArray();
        if (rows.size() > 10000) return false;
        QList<PositionRecord> nextPositions;
        QSet<QString> seen;
        for (const auto& row : rows) {
            const auto wrapper = row.toObject(), position = wrapper.value("position").toObject();
            PositionRecord p;
            p.symbol = position.value("coin").toString();
            p.amount = number(position.value("szi"));
            p.entryPrice = number(position.value("entryPx"));
            p.unrealizedPnl = number(position.value("unrealizedPnl"));
            p.notional = number(position.value("positionValue"));
            p.initialMargin = number(position.value("marginUsed"));
            const auto leverage = position.value("leverage").toObject();
            p.marginType = leverage.value("type").toString();
            p.leverage = leverage.value("value").toString();
            if (!row.isObject() || wrapper.value("type").toString() != "oneWay" || !universe.contains(p.symbol) || seen.contains(p.symbol) ||
                p.amount.isEmpty() || p.unrealizedPnl.isEmpty() || !unsignedAmount(position.value("positionValue")) ||
                !unsignedAmount(position.value("marginUsed")) || !QStringList{"cross", "isolated"}.contains(p.marginType) ||
                !integer.match(p.leverage).hasMatch() || decimal::compare(p.leverage, "1000") > 0) return false;
            const auto magnitude = p.amount.startsWith('-') ? p.amount.mid(1) : p.amount;
            if (decimal::positive(magnitude) && !decimal::positive(p.entryPrice)) return false;
            const auto liquidation = position.value("liquidationPx");
            if (!liquidation.isNull() && !unsignedAmount(liquidation)) return false;
            p.liquidationPrice = liquidation.toString();
            p.positionSide = decimal::normalize(magnitude) == "0" ? "flat" : p.amount.startsWith('-') ? "short" : "long";
            p.marginAsset = "USDC";
            p.updateTime = timeText;
            seen.insert(p.symbol);
            nextPositions.append(p);
        }
        AssetCapture capture;
        auto& snapshot = capture.snapshot;
        snapshot.provider = "phantom"; snapshot.environment = "mainnet"; snapshot.market = "hyperliquid_perps";
        // The same underlying account keeps one scope across registrations and modes.
        snapshot.accountScope = hash(identityDomainText(IdentityDomain::HyperliquidMainnet) + address.toLower());
        snapshot.connectionId = account.id;
        snapshot.timeMs = received;
        snapshot.id = hash(snapshot.accountScope + ":" + QString::number(received) + ":" + hash(QString::fromUtf8(document.toJson(QJsonDocument::Compact))));
        snapshot.balanceComplete = true;
        snapshot.reconciled = false;
        snapshot.coverage = supportedCoverage();
        snapshot.evidence = {"Hyperliquid 기본 USDC 선물 잔고", "https://api.hyperliquid.xyz/info", snapshot.accountScope, source, received, true};
        AssetComponent equity;
        // The API field has no unit annotation. Standard perps account value is
        // explicitly USDC in /trading/builder-codes; /trading/contract-specifications
        // records quanto PnL in USDC. This is nominal equity, not a USD peg quote.
        equity.id = "hyperliquid:usdc:equity"; equity.asset = "USDC";
        equity.kind = AssetComponentKind::Equity; equity.quantity = number(summary.value("accountValue"));
        equity.valuationGroup = "hyperliquid:default:USDC"; equity.evidence = snapshot.evidence;
        snapshot.components.append(equity);
        for (const auto& p : nextPositions) {
            AssetComponent component;
            component.id = "hyperliquid:position:" + p.symbol; component.asset = p.symbol;
            component.kind = AssetComponentKind::PositionNotional;
            component.quantity = p.amount; component.reportedValue = p.notional; component.reportedValueCurrency = "USDC";
            component.providerUnrealizedPnl = p.unrealizedPnl;
            component.valuationGroup = "hyperliquid:positions"; component.evidence = snapshot.evidence;
            snapshot.components.append(component);
        }
        readSourceTime = source;
        if (mode == "unifiedAccount") {
            pending = std::move(capture); pendingPositions = std::move(nextPositions);
            stage = 3; send(); return true;
        }
        commit(std::move(capture), std::move(nextPositions));
        return true;
    }
    QString currentCoverage() const {
        if (mode != "unifiedAccount") return supportedCoverage();
        return "Hyperliquid 통합 계좌 공유 USDC 원본잔고(total)·기본 DEX 포지션. "
            "현물·선물이 공유하는 기초잔고이며 순자산 미확정. hold 차감·미실현손익 가산 안 함. "
            "HIP-3·다른 담보·Portfolio Margin·과거 이력 미포함.";
    }
    void commit(AssetCapture capture, QList<PositionRecord> nextPositions) {
        latest = std::move(capture); positions = std::move(nextPositions);
        lastSourceTime = readSourceTime;
        pending = {}; pendingPositions.clear();
        active = false;
        address.clear(); universe.clear();
        message = mode == "unifiedAccount" ? "조회 완료 · 통합 공유 USDC 원본잔고 · 순자산 미확정"
            : "조회 완료 · Hyperliquid 기본 USDC 선물 · HIP-3·통합계좌 미포함";
        emit q->changed();
    }
    bool publishUnified(const QJsonDocument& document) {
        if (!document.isObject() || !document.object().value("balances").isArray()) return false;
        const auto rows = document.object().value("balances").toArray();
        if (rows.size() > 10000) return false;
        QSet<QString> identities;
        QString total;
        for (const auto& value : rows) {
            if (!value.isObject()) return false;
            const auto row = value.toObject();
            const auto coin = row.value("coin").toString(), tokenId = row.value("token").toString();
            if (coin.isEmpty() || identities.contains(coin)) return false;
            identities.insert(coin);
            if (coin != "USDC" && tokenId != "0") continue;
            if (coin != "USDC" || tokenId != "0" || !total.isEmpty() || !unsignedAmount(row.value("total")) ||
                !unsignedAmount(row.value("hold")) || decimal::compare(row.value("hold").toString(), row.value("total").toString()) > 0) return false;
            total = row.value("total").toString();
        }
        // Missing is not a verified zero balance, including an agent-wallet address.
        if (total.isEmpty() || pending.snapshot.components.isEmpty()) return false;
        const auto received = QDateTime::currentMSecsSinceEpoch();
        if (received <= 0 || received - pending.snapshot.timeMs > 15000) return false;
        auto capture = pending;
        auto& snapshot = capture.snapshot;
        snapshot.timeMs = received; snapshot.balanceComplete = false;
        snapshot.coverage = currentCoverage();
        snapshot.id = hash(snapshot.id + ":" + QString::number(received) + ":" + QString::fromUtf8(document.toJson(QJsonDocument::Compact)));
        // This endpoint provides no source time. Keep only its receipt time;
        // the separate position components retain clearinghouseState's raw time.
        snapshot.evidence = {"Hyperliquid 통합 공유 USDC 원본잔고", "https://api.hyperliquid.xyz/info", snapshot.accountScope, 0, received, true};
        auto& cash = snapshot.components.first();
        cash.id = "hyperliquid:usdc:shared-balance"; cash.kind = AssetComponentKind::Cash;
        cash.quantity = total; cash.evidence = snapshot.evidence;
        cash.valuationGroup = "hyperliquid:shared:USDC";
        commit(std::move(capture), pendingPositions);
        return true;
    }
    void result(quint64 current, const HttpResult& response) {
        if (!active || current != token || !accountCredentialsAvailable(vault, account.id)) return;
        if (response.status == 429 || response.localRetryAfterSeconds > 0) {
            int wait = response.localRetryAfterSeconds > 0 ? response.localRetryAfterSeconds : 60;
            for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it) if (it.key().compare("Retry-After", Qt::CaseInsensitive) == 0) {
                bool ok = false; const auto seconds = QString::fromLatin1(it.value()).toInt(&ok);
                if (ok && seconds > 0) wait = seconds;
            }
            cooldownUntil = clock.elapsed() + qBound(1, wait, 86400) * 1000LL;
            fail("요청 제한 대기 중 · 자동 재전송하지 않습니다."); return;
        }
        if (response.status != 200 || response.networkError || response.timedOut) { fail("공식 읽기 응답을 확인하지 못했습니다."); return; }
        QString error;
        const auto document = parseExactJson(stage == 0 ? QByteArray("[") + response.body + "]" : response.body, &error);
        if (!error.isEmpty()) { fail("응답 JSON 형식 또는 크기를 확인하지 못했습니다."); return; }
        if (stage == 0) {
            const auto values = document.array();
            if (values.size() != 1 || !values[0].isString() || !QStringList{"disabled", "unifiedAccount"}.contains(values[0].toString())) {
                fail("Standard·통합 USDC 원본잔고만 지원합니다. Portfolio Margin·DEX abstraction·미확인 모드는 지원하지 않습니다."); return;
            }
            mode = values[0].toString();
        } else if (stage == 1) {
            if (!metadata(document)) { fail("기본 DEX의 USDC 담보·종목 범위를 확인하지 못했습니다."); return; }
        } else if (stage == 2) {
            if (!publish(document)) fail("선물 잔고·포지션 형식 또는 서버 원본 시각을 확인하지 못했습니다.");
            return;
        } else {
            if (!publishUnified(document)) fail("통합 USDC 원본잔고·보류 수량 또는 지원 범위를 확인하지 못했습니다.");
            return;
        }
        ++stage;
        send();
    }
};

HyperliquidReadController::HyperliquidReadController(Vault& vault, TradingTransport* transport, QObject* parent)
    : QObject(parent), d_(std::make_unique<Private>(this, vault, transport ? transport : new NetworkTradingTransport(this))) {
    connect(d_->transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& result) { d_->result(token, result); });
    connect(&vault, &Vault::aboutToLock, this, &HyperliquidReadController::stop);
    connect(&vault, &Vault::accountsChanged, this, &HyperliquidReadController::stop);
}
HyperliquidReadController::~HyperliquidReadController() { disconnect(this, nullptr, nullptr, nullptr); stop(); }
bool HyperliquidReadController::validAddress(const QString& address) {
    static const QRegularExpression pattern("\\A0x[0-9a-fA-F]{40}\\z");
    return pattern.match(address).hasMatch() && address.mid(2) != QString(40, '0');
}
bool HyperliquidReadController::selectAccount(const QString& id) {
    if (!id.isEmpty() && id == d_->account.id && accountCredentialsAvailable(d_->vault, id)) return true;
    stop(); d_->account = {};
    for (const auto& account : d_->vault.accounts())
        if (account.id == id && account.provider == "phantom" && account.market == "hyperliquid_perps" && account.environment == "mainnet") d_->account = account;
    return !d_->account.id.isEmpty();
}
AccountRecord HyperliquidReadController::account() const { return d_->account; }
bool HyperliquidReadController::refreshAssets() {
    if (d_->active) return true;
    if (!accountCredentialsAvailable(d_->vault, d_->account.id)) return false;
    if (d_->clock.elapsed() < d_->cooldownUntil) { d_->fail("요청 제한 대기 중 · 잠시 후 수동으로 다시 조회하세요."); return false; }
    d_->address.clear();
    d_->vault.withCredentials(d_->account.id, [this](const CredentialSet& credentials) {
        const auto it = credentials.find("address");
        if (it != credentials.end()) d_->address = QString::fromUtf8(reinterpret_cast<const char*>(it->second.data()), it->second.size());
    });
    if (!validAddress(d_->address)) { d_->fail("조회용 Ethereum 주소(0x와 40자리 hex)를 확인하세요. 개인키·시드 문구는 사용하지 않습니다."); return false; }
    d_->address = d_->address.toLower();
    d_->stage = 0; d_->active = true; d_->mode.clear(); d_->universe.clear(); d_->pending = {}; d_->pendingPositions.clear(); d_->send();
    return true;
}
bool HyperliquidReadController::canReconnectAssets() const {
    return !d_->active && accountCredentialsAvailable(d_->vault, d_->account.id) && d_->clock.elapsed() >= d_->cooldownUntil;
}
void HyperliquidReadController::stop() {
    ++d_->token; d_->active = false; d_->transport->abort();
    d_->address.clear(); d_->mode.clear(); d_->universe.clear(); d_->latest = {}; d_->positions.clear();
    d_->pending = {}; d_->pendingPositions.clear(); d_->lastSourceTime = d_->readSourceTime = 0;
    d_->message = "Hyperliquid 선물 조회 중지"; emit changed();
}
bool HyperliquidReadController::interruptRead() {
    if (!d_->active) return false;
    ++d_->token; d_->active = false; d_->transport->abort(); d_->address.clear(); d_->universe.clear();
    d_->pending = {}; d_->pendingPositions.clear();
    d_->message = "Hyperliquid 선물 조회 중단 · 이전 확인 값은 최신 잔고가 아닙니다."; emit changed(); return true;
}
bool HyperliquidReadController::busy() const { return d_->active; }
QString HyperliquidReadController::status() const { return d_->message; }
QString HyperliquidReadController::coverage() const { return d_->currentCoverage(); }
AssetCapture HyperliquidReadController::assetCapture() const { return accountCredentialsAvailable(d_->vault, d_->account.id) ? d_->latest : AssetCapture{}; }
QList<PositionRecord> HyperliquidReadController::positions() const { return accountCredentialsAvailable(d_->vault, d_->account.id) ? d_->positions : QList<PositionRecord>{}; }
}
