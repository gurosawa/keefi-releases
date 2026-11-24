#include "keefi/IdentityCompatibility.h"
#include "keefi/KisAdapter.h"
#include "keefi/ProductIdentity.h"
#include "keefi/KisMasterCatalog.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QTimeZone>
#include <QUrlQuery>
#include <QUuid>
#include <sodium.h>
#include <algorithm>
#include <cstdlib>

namespace keefi {
namespace {
const QTimeZone Seoul("Asia/Seoul");
QString str(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }
bool digits(const QString& s, int lo = 1, int hi = 19) {
    if (s.size() < lo || s.size() > hi) return false;
    for (const auto c : s) if (c < '0' || c > '9') return false;
    return true;
}
bool num(const QString& s) { return s.size() <= 64 && decimal::valid(s); }
bool positive(const QString& s) { return num(s) && decimal::positive(s); }
bool ascii(const QString& s, int max) {
    if (s.isEmpty() || s.size() > max) return false;
    for (auto c : s) if (c.unicode() < 33 || c.unicode() > 126) return false;
    return true;
}
bool symbol(const QString& s) {
    static const QRegularExpression re(QStringLiteral("\\A(?:[0-9A-Z]{6}|Q[0-9A-Z]{6})\\z"));
    return re.match(s).hasMatch() && (s.size() == 6 || s.size() == 7);
}
struct Ref { QString date, branch, number, venue; };
Ref ref(const QString& s) {
    const auto p = s.split('_');
    if (p.size() != 5 || p[0] != "K1" || !digits(p[1], 8, 8) || !QDate::fromString(p[1], "yyyyMMdd").isValid() ||
        !digits(p[2], 1, 5) || !digits(p[3], 1, 10) || !positive(p[3]) || !QStringList{"KRX", "NXT", "SOR"}.contains(p[4])) return {};
    return {p[1], p[2], p[3], p[4]};
}
QString makeRef(const QString& date, const QString& branch, const QString& number, const QString& venue) {
    const QString s = "K1_" + date + '_' + branch + '_' + number + '_' + venue;
    return ref(s).date.isEmpty() ? QString() : s;
}
QByteArray secretBytes(const SecureBuffer& b) { return QByteArray(reinterpret_cast<const char*>(b.data()), qsizetype(b.size())); }
QString secretText(const SecureBuffer& b) { return QString::fromUtf8(reinterpret_cast<const char*>(b.data()), qsizetype(b.size())); }
void wipe(QByteArray& b) { if (!b.isEmpty()) { b.detach(); sodium_memzero(b.data(), std::size_t(b.size())); } b.clear(); }
void wipe(QString& s) { if (!s.isEmpty()) { s.detach(); sodium_memzero(s.data(), std::size_t(s.size()) * sizeof(QChar)); } s.clear(); }
const SecureBuffer* credential(const CredentialSet& c, const QString& k) { auto i = c.find(k); return i == c.end() ? nullptr : &i->second; }
QString digest(const QByteArray& domain, const QList<QByteArray>& parts) {
    QCryptographicHash h(QCryptographicHash::Sha256); h.addData(domain);
    for (const auto& p : parts) {
        const quint32 n = quint32(p.size()); const char length[] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
        h.addData(QByteArrayView(length, 4)); h.addData(p);
    }
    return QString::fromLatin1(h.result().toHex());
}
QByteArray binding(const BrokerStep& s) {
    return QCryptographicHash::hash(QJsonDocument(QJsonObject{{"op", int(s.operation)}, {"variant", s.variant}, {"symbol", s.input.symbol},
        {"side", s.input.side}, {"type", s.input.type}, {"price", s.input.price}, {"quantity", s.input.quantity},
        {"order", s.input.orderId}, {"start", QString::number(s.input.startTimeMs)}, {"end", QString::number(s.input.endTimeMs)}}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}
AdapterResult failed(Operation op, const QString& message, bool unknown = false) {
    AdapterResult r; r.code = "kis-failed"; r.message = message; r.unknown = isMutation(op) && unknown; r.needsReconciliation = r.unknown; return r;
}
BrokerReadiness denied(BrokerNotReady reason, const QString& message) { return {false, reason, message}; }
QString tick(const QString& price, const QString& group) {
    if (!positive(price)) return {};
    if (group == "EF" || group == "EN") return decimal::compare(price, "2000") < 0 ? "1" : "5";
    if (group != "ST") return {};
    const char* bounds[] = {"2000", "5000", "20000", "50000", "200000", "500000"};
    const char* steps[] = {"1", "5", "10", "50", "100", "500"};
    for (int i = 0; i < 6; ++i) if (decimal::compare(price, bounds[i]) < 0) return steps[i];
    return "1000";
}
QString defaultVariant(Operation op) {
    switch (op) {
    case Operation::Authenticate: return "token";
    case Operation::Holdings: case Operation::Account: return "balance";
    case Operation::BuyingPower: return "cash-exact";
    case Operation::SellableQuantity: return "live-sellable";
    case Operation::Rules: case Operation::Quote: return "krx-price";
    case Operation::Depth: return "krx-book";
    case Operation::TradingSchedule: return "open-day";
    case Operation::Reference: return "dated-minute";
    case Operation::OpenOrders: return "daily-open";
    case Operation::History: return "daily-all";
    case Operation::QueryOrder: return "order-reference";
    default: return {};
    }
}
QByteArray header(const HttpResult& r, const QByteArray& key) {
    for (auto i = r.headers.begin(); i != r.headers.end(); ++i) if (i.key().toLower() == key) return i.value();
    return {};
}
bool validText(const QString& s, int max = 160) {
    if (s.size() > max) return false;
    for (auto c : s) if (c.category() == QChar::Other_Control || c.isNull()) return false;
    return true;
}
bool optionalBusinessDate(const QString& s) {
    if (s.size() > 8) return false;
    if (s.isEmpty() || std::all_of(s.cbegin(), s.cend(), [](QChar c) { return c == ' '; })) return true;
    return digits(s, 8, 8) && QDate::fromString(s, "yyyyMMdd").isValid();
}
} // namespace

KisAdapter::KisAdapter(QString environment, std::function<QDateTime()> utcNow)
    : environment_(std::move(environment)), utcNow_(std::move(utcNow)) {}
KisAdapter::~KisAdapter() { clearState(); }
void KisAdapter::clearState() const {
    token_.clear(); appKey_.clear(); appSecret_.clear(); cano_.clear(); product_.clear();
    tokenClock_.invalidate(); lifetimeMs_ = 0; selected_ = false; handle_.clear(); scope_.clear(); label_.clear();
    cursors_.clear(); mutationDate_.clear(); mutationTargets_.clear();
}
void KisAdapter::clearSession() { clearState(); }
QString KisAdapter::origin() const {
    if (environment_ == "live") return "https://openapi.koreainvestment.com:9443";
    return {};
}
bool KisAdapter::symbolValid(const QString& s) const { return symbol(s); }
bool KisAdapter::catalogSymbolValid(const QString& s) const {
    static const QRegularExpression re(QStringLiteral("\\A[0-9A-Z]{6,9}\\z"));
    return re.match(s).hasMatch();
}
QString KisAdapter::orderReference(const QString& date, const QString& branch, const QString& number, const QString& venue) { return makeRef(date, branch, number, venue); }
bool KisAdapter::orderReferenceValid(const QString& s) const { return !ref(s).date.isEmpty(); }
bool KisAdapter::sessionReady() const {
    if (!tokenClock_.isValid() || token_.empty() || tokenClock_.elapsed() >= lifetimeMs_) {
        if (tokenClock_.isValid()) clearState();
        return false;
    }
    return true;
}
QString KisAdapter::credentialIdentityFingerprint(const CredentialSet& c) const {
    const auto* k = credential(c, "app_key");
    if (!k || k->empty()) return {};
    auto b = secretBytes(*k); const auto guard = qScopeGuard([&] { wipe(b); });
    return digest(identityDomain(IdentityDomain::KisCredentialIdentity), {environment_.toUtf8(), b});
}
bool KisAdapter::configureSession(const AccountRecord& a, const CredentialSet& c, QString* error) {
    clearState();
    auto fail = [&] { if (error) *error = QStringLiteral("한국투자증권 환경·App Key·App Secret·계좌 8자리·상품코드 2자리를 확인하세요."); clearState(); return false; };
    if (origin().isEmpty() || a.provider != "kis" || a.environment != environment_ || a.market != "domestic_equity" || a.currency != "KRW") return fail();
    const auto* k = credential(c, "app_key"); const auto* s = credential(c, "app_secret");
    const auto* n = credential(c, "account_number"); const auto* p = credential(c, "product_code");
    if (!k || !s || !n || !p) return fail();
    QString key = secretText(*k), secret = secretText(*s), account = secretText(*n), product = secretText(*p);
    const auto guard = qScopeGuard([&] { wipe(key); wipe(secret); wipe(account); wipe(product); });
    if (!ascii(key, 256) || !ascii(secret, 512) || !digits(account, 8, 8) || !digits(product, 2, 2)) return fail();
    try {
        appKey_ = SecureBuffer(k->data(), k->size()); appSecret_ = SecureBuffer(s->data(), s->size());
        cano_ = SecureBuffer(n->data(), n->size()); product_ = SecureBuffer(p->data(), p->size());
        QByteArray nb = account.toUtf8(), pb = product.toUtf8();
        const auto cleanup = qScopeGuard([&] { wipe(nb); wipe(pb); });
        scope_ = digest(identityDomain(IdentityDomain::BrokerConfiguredIdentity), {QByteArray("kis"), environment_.toUtf8(), nb, pb});
        handle_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        label_ = QStringLiteral("한국투자증권 ••••%1 · %2 · 등록 계좌").arg(account.right(4), QStringLiteral("실전"));
    } catch (...) { return fail(); }
    return true;
}
QList<BrokerAccountChoice> KisAdapter::accountChoices() const { return sessionReady() ? QList<BrokerAccountChoice>{{handle_, label_}} : QList<BrokerAccountChoice>{}; }
bool KisAdapter::selectBrokerAccount(const QString& h, QString* error) {
    if (!sessionReady() || h != handle_ || h.isEmpty()) { if (error) *error = QStringLiteral("이번 연결의 등록 계좌를 선택하세요."); return false; }
    selected_ = true; cursors_.clear(); return true;
}
QString KisAdapter::selectedAccountScope() const { return sessionReady() && selected_ ? scope_ : QString(); }
QString KisAdapter::selectedAccountLabel() const { return sessionReady() && selected_ ? label_ : QString(); }
bool KisAdapter::bindMutationTarget(const QString& operation, const QString& original) const {
    if (selectedAccountScope().isEmpty() || operation == original) return false;
    const auto child = ref(operation), parent = ref(original);
    if (child.date.isEmpty() || parent.date.isEmpty() || child.date != parent.date ||
        child.venue != "KRX" || parent.venue != "KRX") return false;
    const auto existing = mutationTargets_.constFind(operation);
    if (existing != mutationTargets_.cend()) return existing.value() == original;
    if (mutationTargets_.size() >= 1000) return false;
    mutationTargets_.insert(operation, original);
    return true;
}
bool KisAdapter::restoreReconciliationIntent(const QString& kind, const OrderInput& in) {
    // This input is a controller-verified encrypted journal record, never an
    // arbitrary user association. Binding context alone proves no server state.
    if (kind != "cancel" && kind != "amend") return false;
    return bindMutationTarget(in.operationOrderId, in.orderId);
}

BrokerPolicy KisAdapter::policy() const {
    BrokerPolicy p; p.provider = "kis"; p.displayName = QStringLiteral("한국투자증권"); p.environment = environment_;
    p.quoteOperation = Operation::Rules; p.partialFillAmend = true;
    p.accountMode = BrokerAccountMode::Configured; p.exactIntentPreflight = true; p.publicCatalog = true;
    for (auto action : {BrokerAction::Read, BrokerAction::Place, BrokerAction::Cancel, BrokerAction::Amend})
        p.actions.insert(action, {ApiAvailability::Documented, true, QStringLiteral("공식 API 제공 · 입력별 안전 조건과 실계좌 검증 별도")});
    p.catalog = {ApiAvailability::Documented, true, QStringLiteral("KIS 공식 종목 마스터")};
    p.officialOrderTest = {ApiAvailability::NotDocumented, false, QStringLiteral("비체결 주문 시험 API는 확인되지 않았습니다.")};
    p.paperTrading = {ApiAvailability::Documented, false, QStringLiteral("%1는 실전 연결만 지원합니다(모의 지원 종료).").arg(productName())};
    p.catalogSource = "KIS 공식 KOSPI/KOSDAQ/KONEX master ZIP";
    p.holdingsSource = "KIS inquire-balance"; p.buyingPowerSource = "KIS inquire-psbl-order · 미수 없는 한도 · 수수료 포함 여부 미확인";
    p.freshness[BrokerAction::Place] = {{Operation::Rules, 10000, SourceTimeQuality::NotProvided, true},
        {Operation::Depth, 10000, SourceTimeQuality::TimeOfDayOnly, true}};
    for (auto op : {Operation::BuyingPower, Operation::SellableQuantity, Operation::Holdings, Operation::TradingSchedule, Operation::Reference})
        p.freshness[BrokerAction::Place].append({op, 10000, SourceTimeQuality::NotProvided, false});
    p.freshness[BrokerAction::Amend] = p.freshness.value(BrokerAction::Place);
    p.freshness[BrokerAction::Amend].append({Operation::OpenOrders, 10000, SourceTimeQuality::NotProvided, true});
    p.freshness[BrokerAction::Cancel] = {{Operation::OpenOrders, 10000, SourceTimeQuality::NotProvided, true}};
    return p;
}
QList<Operation> KisAdapter::refreshPlan() const { return {Operation::OpenOrders, Operation::History, Operation::Holdings, Operation::Rules, Operation::Depth}; }
QList<BrokerStep> KisAdapter::plan(BrokerPlanKind kind, const OrderInput& in) const {
    QList<BrokerStep> result;
    auto add = [&](Operation op, const QString& variant = QString(), BrokerAuthScope auth = BrokerAuthScope::SelectedAccount) {
        BrokerStep s; s.operation = op; s.authScope = auth; s.variant = variant.isEmpty() ? defaultVariant(op) : variant;
        s.input = directStep(op, in).input; s.minimumDelayMs = 300; result.push_back(s);
    };
    if (kind == BrokerPlanKind::Authenticate) { add(Operation::Authenticate, "token", BrokerAuthScope::CredentialBootstrap); return result; }
    if (kind == BrokerPlanKind::Catalog) {
        for (const auto& m : {"KOSPI", "KOSDAQ", "KONEX"}) {
            add(Operation::Markets, "master-zip", BrokerAuthScope::Public); result.last().catalogMarket = m;
        }
        return result;
    }
    if (kind == BrokerPlanKind::CancelPreflight) {
        add(Operation::OpenOrders, "amendable-orders");
        return result;
    }
    if (kind == BrokerPlanKind::AccountRead) {
        add(Operation::OpenOrders); add(Operation::History); add(Operation::Holdings);
        if (symbol(in.symbol)) { add(Operation::Rules); add(Operation::Depth); }
        return result;
    }
    if (!symbol(in.symbol)) return result;
    if (!in.orderId.isEmpty()) add(Operation::OpenOrders, "amendable-orders");
    if (in.side == "buy") { add(Operation::BuyingPower, "cash-market-cap"); if (in.type == "limit") add(Operation::BuyingPower, "cash-exact"); }
    else if (in.side == "sell") {
        add(Operation::Holdings, "cash-holdings");
        add(Operation::SellableQuantity, "live-sellable");
    }
    add(Operation::TradingSchedule);
    add(Operation::Rules); add(Operation::Depth);
    return result;
}

BrokerStep KisAdapter::directStep(Operation op, const OrderInput& input) const {
    BrokerStep s; s.operation = op; s.variant = defaultVariant(op); s.input = input;
    s.minimumDelayMs = 300;
    s.authScope = op == Operation::Authenticate ? BrokerAuthScope::CredentialBootstrap : BrokerAuthScope::SelectedAccount;
    const auto now = utcNow_().toTimeZone(Seoul);
    if (op == Operation::OpenOrders || op == Operation::History) {
        if (!s.input.endTimeMs) s.input.endTimeMs = now.toMSecsSinceEpoch();
        if (!s.input.startTimeMs) s.input.startTimeMs = QDateTime(now.date().addDays(op == Operation::History ? -6 : 0), QTime(0,0), Seoul).toMSecsSinceEpoch();
    }
    return s;
}
RequestSpec KisAdapter::build(Operation op, const AccountRecord&, const OrderInput& input, const CredentialSet&, qint64 now, QString* error) const {
    BrokerStep s; s.operation = op; s.variant = defaultVariant(op); s.input = input;
    s.authScope = op == Operation::Authenticate ? BrokerAuthScope::CredentialBootstrap : BrokerAuthScope::SelectedAccount;
    return buildStep(s, now, error);
}
RequestSpec KisAdapter::buildStep(const BrokerStep& step, qint64 nowMs, QString* error) const {
    Q_UNUSED(nowMs); // No signature timestamp: all KIS calendar context uses the session clock seam.
    RequestSpec r; r.operation = step.operation;
    auto fail = [&](const QString& e) { if (error) *error = e; return RequestSpec{}; };
    if (origin().isEmpty()) return fail(QStringLiteral("한국투자증권 환경이 올바르지 않습니다."));
    if (step.operation == Operation::Markets) {
        if (step.variant != "master-zip" || !QStringList{"KOSPI", "KOSDAQ", "KONEX"}.contains(step.catalogMarket)) return fail(QStringLiteral("종목 목록 요청이 올바르지 않습니다."));
        r.url = QUrl(KisMasterCatalog::sourceUrl(step.catalogMarket)); return r;
    }
    r.authenticated = true; r.headers.insert("Content-Type", "application/json; charset=utf-8");
    if (step.operation == Operation::Authenticate) {
        if (appKey_.empty() || appSecret_.empty() || step.variant != "token") return fail(QStringLiteral("등록된 자격 증명을 다시 확인하세요."));
        auto k = secretText(appKey_), s = secretText(appSecret_); const auto cleanup = qScopeGuard([&] { wipe(k); wipe(s); });
        r.method = "POST"; r.url = QUrl(origin() + "/oauth2/tokenP");
        r.body = QJsonDocument(QJsonObject{{"grant_type", "client_credentials"}, {"appkey", k}, {"appsecret", s}}).toJson(QJsonDocument::Compact); return r;
    }
    if (!sessionReady() || !selected_) return fail(QStringLiteral("등록 계좌를 인증하고 선택하세요."));
    r.headers.insert("authorization", "Bearer " + secretBytes(token_)); r.headers.insert("appkey", secretBytes(appKey_));
    r.headers.insert("appsecret", secretBytes(appSecret_)); r.headers.insert("custtype", "P");
    QUrlQuery q; QJsonObject body; QString path, tr;
    auto account = [&] { q.addQueryItem("CANO", secretText(cano_)); q.addQueryItem("ACNT_PRDT_CD", secretText(product_)); };
    const auto& in = step.input; const auto now = utcNow_().toTimeZone(Seoul);
    if (!now.isValid()) return fail(QStringLiteral("요청 시각을 확인할 수 없습니다."));
    if (step.operation == Operation::Rules || step.operation == Operation::Quote || step.operation == Operation::Depth || step.operation == Operation::Reference) {
        if (!symbol(in.symbol)) return fail(QStringLiteral("국내 종목코드를 확인하세요."));
        q.addQueryItem("FID_COND_MRKT_DIV_CODE", "J"); q.addQueryItem("FID_INPUT_ISCD", in.symbol);
        if (step.operation == Operation::Depth) { path = "inquire-asking-price-exp-ccn"; tr = "FHKST01010200"; }
        else if (step.operation == Operation::Reference) {
            path = "inquire-time-itemchartprice"; tr = "FHKST03010200";
            q.addQueryItem("FID_INPUT_HOUR_1", now.time().toString("HHmmss")); q.addQueryItem("FID_PW_DATA_INCU_YN", "N"); q.addQueryItem("FID_ETC_CLS_CODE", "");
        } else { path = "inquire-price"; tr = "FHKST01010100"; }
        path = "/uapi/domestic-stock/v1/quotations/" + path;
    } else if (step.operation == Operation::TradingSchedule) {
        path = "/uapi/domestic-stock/v1/quotations/chk-holiday"; tr = "CTCA0903R";
        q.addQueryItem("BASS_DT", now.date().toString("yyyyMMdd")); q.addQueryItem("CTX_AREA_NK", ""); q.addQueryItem("CTX_AREA_FK", "");
    } else if (step.operation == Operation::Holdings || step.operation == Operation::Account) {
        account(); path = "/uapi/domestic-stock/v1/trading/inquire-balance"; tr = "TTTC8434R";
        for (const auto& pair : QList<QPair<QString, QString>>{{"AFHR_FLPR_YN", "N"}, {"OFL_YN", ""}, {"INQR_DVSN", "01"}, {"UNPR_DVSN", "01"},
            {"FUND_STTL_ICLD_YN", "N"}, {"FNCG_AMT_AUTO_RDPT_YN", "N"}, {"PRCS_DVSN", "00"}}) q.addQueryItem(pair.first, pair.second);
    } else if (step.operation == Operation::BuyingPower) {
        if (!symbol(in.symbol) || (step.variant != "cash-exact" && step.variant != "cash-market-cap") ||
            (in.type != "limit" && in.type != "market") || (in.type == "limit" && !positive(in.price))) return fail(QStringLiteral("매수 가능 조회에 주문 조건이 필요합니다."));
        account(); path = "/uapi/domestic-stock/v1/trading/inquire-psbl-order"; tr = "TTTC8908R";
        const bool market = step.variant == "cash-market-cap" || in.type == "market";
        q.addQueryItem("PDNO", in.symbol); q.addQueryItem("ORD_DVSN", market ? "01" : "00"); q.addQueryItem("ORD_UNPR", market ? "0" : in.price);
        q.addQueryItem("CMA_EVLU_AMT_ICLD_YN", "N"); q.addQueryItem("OVRS_ICLD_YN", "N");
    } else if (step.operation == Operation::SellableQuantity) {
        if (!symbol(in.symbol)) return fail(QStringLiteral("공식 매도 가능 수량 API는 이 환경에서 사용할 수 없습니다."));
        account(); path = "/uapi/domestic-stock/v1/trading/inquire-psbl-sell"; tr = "TTTC8408R"; q.addQueryItem("PDNO", in.symbol);
    } else if (step.operation == Operation::OpenOrders && step.variant == "amendable-orders") {
        account(); path = "/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl"; tr = "TTTC0084R";
        q.addQueryItem("INQR_DVSN_1", "0"); q.addQueryItem("INQR_DVSN_2", "0");
    } else if (step.operation == Operation::OpenOrders || step.operation == Operation::History || step.operation == Operation::QueryOrder) {
        account(); path = "/uapi/domestic-stock/v1/trading/inquire-daily-ccld";
        QDate from = now.date(), to = now.date(); Ref selected;
        if (step.operation == Operation::QueryOrder) {
            selected = ref(in.orderId); if (selected.date.isEmpty()) return fail(QStringLiteral("날짜·지점·주문번호·거래소가 포함된 주문 참조가 필요합니다."));
            from = to = QDate::fromString(selected.date, "yyyyMMdd");
        } else if (step.operation == Operation::History || step.operation == Operation::OpenOrders) {
            from = in.startTimeMs > 0 ? QDateTime::fromMSecsSinceEpoch(in.startTimeMs, Qt::UTC).toTimeZone(Seoul).date() : now.date().addDays(step.operation == Operation::History ? -6 : 0);
            to = in.endTimeMs > 0 ? QDateTime::fromMSecsSinceEpoch(in.endTimeMs, Qt::UTC).toTimeZone(Seoul).date() : now.date();
        }
        if (!from.isValid() || !to.isValid() || from > to || to > now.date() || from.daysTo(to) > 90) return fail(QStringLiteral("주문 조회 기간을 90일 이내로 지정하세요."));
        const bool old = to < now.date().addMonths(-3);
        if (!old && from < now.date().addMonths(-3)) return fail(QStringLiteral("3개월 경계를 나누어 조회하세요."));
        tr = old ? "CTSC9215R" : "TTTC0081R";
        q.addQueryItem("INQR_STRT_DT", from.toString("yyyyMMdd")); q.addQueryItem("INQR_END_DT", to.toString("yyyyMMdd"));
        q.addQueryItem("SLL_BUY_DVSN_CD", "00"); q.addQueryItem("INQR_DVSN", "01"); q.addQueryItem("PDNO", "");
        q.addQueryItem("CCLD_DVSN", step.operation == Operation::OpenOrders ? "02" : "00");
        q.addQueryItem("ORD_GNO_BRNO", selected.branch); q.addQueryItem("ODNO", selected.number);
        q.addQueryItem("INQR_DVSN_1", ""); q.addQueryItem("INQR_DVSN_3", "01");
        q.addQueryItem("EXCG_ID_DVSN_CD", selected.venue.isEmpty() ? "ALL" : selected.venue);
    } else  return fail(QStringLiteral("이 한국투자증권 API 동작은 구현되지 않았습니다."));
    const bool paged = step.operation == Operation::Holdings || step.operation == Operation::Account || step.operation == Operation::OpenOrders || step.operation == Operation::History || step.operation == Operation::QueryOrder;
    if (paged) {
        QString fk, nk;
        if (!step.cursorHandle.isEmpty()) {
            auto found = cursors_.find(step.cursorHandle);
            if (found == cursors_.end() || found->second.operation != step.operation || found->second.variant != step.variant || found->second.binding != binding(step))
                return fail(QStringLiteral("연속 조회 문맥이 만료되었거나 달라졌습니다."));
            fk = secretText(found->second.fk); nk = secretText(found->second.nk); r.headers.insert("tr_cont", "N");
        }
        q.addQueryItem("CTX_AREA_FK100", fk); q.addQueryItem("CTX_AREA_NK100", nk); wipe(fk); wipe(nk);
    }
    r.headers.insert("tr_id", tr.toLatin1()); r.url = QUrl(origin() + path); if (!q.isEmpty()) r.url.setQuery(q); return r;
}

AdapterResult KisAdapter::parse(Operation op, const HttpResult& http, const OrderInput& input) const {
    BrokerStep step; step.operation = op; step.variant = defaultVariant(op); step.input = input;
    return parseStep(step, http).value;
}
BrokerStepResult KisAdapter::parseStep(const BrokerStep& step, const HttpResult& http) const {
    BrokerStepResult out; const auto op = step.operation;
    auto bad = [&](const QString& message = QStringLiteral("한국투자증권 응답을 안전하게 해석하지 못했습니다."), bool ambiguous = true) {
        out.value = failed(op, message, ambiguous); return out;
    };
    if (http.status == 401) { clearState(); out = bad(QStringLiteral("인증이 만료되었거나 거부되었습니다. 다시 연결하세요.")); out.value.authenticationFailed = true; return out; }
    if (http.timedOut || http.networkError || http.status >= 500 || http.status <= 0) return bad(QStringLiteral("응답을 확인하지 못했습니다. 주문은 재전송하지 않고 상태를 확인합니다."));
    if (http.status == 429) { out = bad(QStringLiteral("요청 제한으로 중지했습니다. 잠시 뒤 다시 조회하세요.")); out.value.rateLimited = true; out.value.retryAfterSeconds = 60; return out; }
    if (op == Operation::Markets) {
        if (http.status != 200) return bad();
        QString error; const auto list = KisMasterCatalog::parseZip(http.body, step.catalogMarket, &error);
        if (!error.isEmpty() || list.isEmpty()) return bad(QStringLiteral("공식 종목 마스터를 읽지 못했습니다."));
        QJsonArray rows;
        for (const auto& item : list) {
            catalog_[item.symbol] = item;
            rows.append(QJsonObject{{"symbol", item.symbol}, {"baseAsset", item.baseAsset}, {"quoteAsset", item.quoteAsset},
                {"name", item.name}, {"englishName", item.englishName}, {"status", item.status}, {"tradable", item.tradable},
                {"warning", item.warning}, {"market", item.market}, {"securityType", item.securityType}});
        }
        out.value.ok = true; out.value.data = {{"instruments", rows}, {"market", step.catalogMarket}}; return out;
    }
    QString parseError; const auto doc = parseExactJson(http.body, &parseError);
    if (!parseError.isEmpty() || !doc.isObject()) return bad();
    const auto raw = doc.object();
    if (op == Operation::Authenticate) {
        if (http.status != 200) { clearState(); return bad(QStringLiteral("한국투자증권 인증 요청이 거부되었습니다."), false); }
        auto token = str(raw, "access_token"); const auto cleanup = qScopeGuard([&] { wipe(token); });
        const auto lifetime = str(raw, "expires_in");
        static const QRegularExpression bearer("\\A[A-Za-z0-9._~+/-]+=*\\z");
        bool good = false; const auto seconds = lifetime.toLongLong(&good);
        if (!ascii(token, 8192) || !bearer.match(token).hasMatch() || str(raw, "token_type").compare("Bearer", Qt::CaseInsensitive) ||
            !good || seconds <= 0 || seconds > 86400 || appKey_.empty() || cano_.empty()) { clearState(); return bad(); }
        qint64 duration = seconds * 1000;
        const auto expiry = str(raw, "access_token_token_expired");
        if (!expiry.isEmpty()) {
            const auto value = QDateTime(QDate::fromString(expiry.left(10), "yyyy-MM-dd"), QTime::fromString(expiry.mid(11), "HH:mm:ss"), Seoul);
            if (expiry.size() != 19 || !value.isValid()) { clearState(); return bad(); }
            duration = std::min(duration, utcNow_().msecsTo(value));
        }
        if (duration <= 1000) { clearState(); return bad(QStringLiteral("받은 인증 토큰이 이미 만료되었습니다."), false); }
        try { token_ = SecureBuffer::fromUtf8(token); } catch (...) { clearState(); return bad(QStringLiteral("보호 메모리를 확보하지 못해 인증을 중지했습니다."), false); }
        lifetimeMs_ = duration - 1000; tokenClock_.start(); out.value.ok = true;
        out.value.data = {{"authenticated", true}, {"accountMode", "configured"}}; return out;
    }
    if (http.status < 200 || http.status >= 300 || str(raw, "rt_cd") != "0") {
        const auto error = str(raw, "msg_cd");
        if (QStringList{"EGW00123", "EGW00121"}.contains(error)) { clearState(); out = bad(QStringLiteral("인증이 만료되었습니다. 다시 연결하세요.")); out.value.authenticationFailed = true; return out; }
        if (error == "EGW00201") { out = bad(QStringLiteral("요청 한도를 초과했습니다.")); out.value.rateLimited = true; out.value.retryAfterSeconds = 60; return out; }
        // Structured rejection is different from an unreadable mutation reply.
        if (!str(raw, "rt_cd").isEmpty() && str(raw, "rt_cd") != "0") return bad(QStringLiteral("한국투자증권 요청이 실패했습니다. 주문 결과는 별도 확인이 필요합니다."), isMutation(op));
        return bad();
    }
    if (!sessionReady() || !selected_) return bad(QStringLiteral("현재 계좌 세션이 유효하지 않습니다."));
    auto result = raw.value("output").toObject();
    if (op == Operation::Holdings || op == Operation::Account) {
        if (!raw.value("output1").isArray() || !raw.value("output2").isArray() || raw.value("output2").toArray().size() != 1) return bad();
        QJsonArray rows; QSet<QString> positions;
        for (auto v : raw.value("output1").toArray()) {
            if (!v.isObject()) return bad(); const auto o = v.toObject();
            const auto id = str(o, "pdno"), quantity = str(o, "hldg_qty"), available = str(o, "ord_psbl_qty"), amount = str(o, "evlu_amt");
            if (!symbol(id) || !digits(quantity) || !digits(available) || !num(amount) || !validText(str(o, "prdt_name"))) return bad();
            const auto trade = str(o, "trad_dvsn_name"), loanDate = str(o, "loan_dt"), expiryDate = str(o, "expd_dt"), loanAmount = str(o, "loan_amt");
            if (!o.value("trad_dvsn_name").isString() || !o.value("loan_dt").isString() || !o.value("expd_dt").isString() ||
                !validText(trade, 60) || !optionalBusinessDate(loanDate) || !optionalBusinessDate(expiryDate) || !num(loanAmount)) return bad();
            // INQR_DVSN=01 can return several loan-date rows for one symbol.
            // This conservative identity is not an exchange-guaranteed row ID.
            // Ignore fixed-width surrounding spaces, never use array position.
            const auto position = digest(identityDomain(IdentityDomain::KisHoldingIdentity), {id.toUtf8(), trade.trimmed().toUtf8(), loanDate.trimmed().toUtf8(), expiryDate.trimmed().toUtf8()});
            if (positions.contains(position)) return bad();
            positions.insert(position);
            rows.append(QJsonObject{{"positionKey", position}, {"symbol", id}, {"name", str(o, "prdt_name")}, {"quantity", quantity}, {"sellableQuantity", available},
                {"currency", "KRW"}, {"cashHolding", loanDate.trimmed().isEmpty() && decimal::compare(loanAmount, "0") == 0}, {"marketValue", QJsonObject{{"amount", amount}}}});
        }
        const auto summary = raw.value("output2").toArray().first().toObject(); const auto cash = str(summary, "dnca_tot_amt");
        // Deposit may be negative on pre-existing margin accounts. It is display data, never buying power.
        static const QRegularExpression signedDecimal("\\A-?[0-9]+(?:\\.[0-9]+)?\\z");
        if (cash.size() > 64 || !signedDecimal.match(cash).hasMatch()) return bad();
        out.value.data = {{"items", rows}, {"depositCash", cash}, {"currency", "KRW"}, {"scope", "국내 주식 잔고 · 예수금과 주문 가능 한도 별도"}};
    } else if (op == Operation::BuyingPower) {
        const auto amount = str(result, "nrcvb_buy_amt"), qty = str(result, "nrcvb_buy_qty"), cash = str(result, "ord_psbl_cash");
        if (!num(amount) || !digits(qty) || !num(cash)) return bad();
        out.value.data = {{"symbol", step.input.symbol}, {"currency", "KRW"}, {"noMarginBuyingPower", amount}, {"noMarginQuantity", qty},
            {"cashBuyingPower", cash}, {"feeKnown", false}, {"feeIncludedInLimitKnown", false}, {"variant", step.variant}};
    } else if (op == Operation::SellableQuantity) {
        QJsonObject row;
        if (raw.value("output1").isObject()) row = raw.value("output1").toObject();
        else if (raw.value("output1").isArray() && raw.value("output1").toArray().size() == 1) row = raw.value("output1").toArray().first().toObject();
        if (str(row, "pdno") != step.input.symbol || !digits(str(row, "ord_psbl_qty"))) return bad();
        out.value.data = {{"symbol", step.input.symbol}, {"sellableQuantity", str(row, "ord_psbl_qty")}};
    } else if (op == Operation::Rules || op == Operation::Quote) {
        const auto id = str(result, "stck_shrn_iscd");
        if (id != step.input.symbol && !(step.input.symbol.startsWith('Q') && id == step.input.symbol.mid(1))) return bad();
        QJsonObject clean{{"symbol", step.input.symbol}, {"currency", "KRW"}};
        for (const auto& pair : QList<QPair<QString, QString>>{{"stck_prpr", "lastPrice"}, {"stck_mxpr", "upperLimitPrice"}, {"stck_llam", "lowerLimitPrice"},
            {"aspr_unit", "tickSize"}, {"hts_deal_qty_unit_val", "quantityStep"}}) {
            const auto s = result.value(pair.first).toString(); if (!num(s)) return bad(); clean.insert(pair.second, s);
        }
        for (const auto* field : {"temp_stop_yn", "sltr_yn", "short_over_yn"}) {
            const auto v = result.value(QLatin1String(field)).toString(); if (!v.isEmpty() && v != "Y" && v != "N") return bad(); clean.insert(QLatin1String(field), v);
        }
        clean.insert("statusCode", str(result, "iscd_stat_cls_code")); clean.insert("viCode", str(result, "vi_cls_code"));
        // Optional market-map fields: previous-day change rate (%) and accumulated trade value (KRW).
        static const QRegularExpression signedNumber(QStringLiteral("\\A-?[0-9]{1,20}(?:\\.[0-9]{1,10})?\\z"));
        if (signedNumber.match(result.value("prdy_ctrt").toString()).hasMatch()) clean.insert("changeRate", result.value("prdy_ctrt").toString());
        if (num(result.value("acml_tr_pbmn").toString())) clean.insert("tradeValue", result.value("acml_tr_pbmn").toString());
        const auto catalog = catalog_.value(step.input.symbol);
        clean.insert("securityType", catalog.securityType); clean.insert("catalogTradable", catalog.tradable); clean.insert("catalogMarket", catalog.market);
        clean.insert("timestamp", QJsonValue::Null); out.value.data = clean;
    } else if (op == Operation::Depth) {
        if (!raw.value("output1").isObject()) return bad(); const auto book = raw.value("output1").toObject();
        const auto time = str(book, "aspr_acpt_hour"), state = str(book, "new_mkop_cls_code");
        if (!digits(time, 6, 6) || !QTime::fromString(time, "HHmmss").isValid() || !digits(state, 2, 2)) return bad();
        QJsonArray asks, bids;
        for (int i = 1; i <= 10; ++i) {
            for (const auto& side : {QString("ask"), QString("bid")}) {
                const auto p = book.value(side + "p" + QString::number(i)).toString();
                const auto q = book.value(side + "p_rsqn" + QString::number(i)).toString();
                if (!num(p) || !digits(q)) return bad();
                if (positive(p) && positive(q)) (side == "ask" ? asks : bids).append(QJsonObject{{"price", p}, {"quantity", q}});
            }
        }
        out.sourceTime = {SourceTimeQuality::TimeOfDayOnly, time, "aspr_acpt_hour", {}};
        out.value.data = {{"symbol", step.input.symbol}, {"currency", "KRW"}, {"asks", asks}, {"bids", bids},
            {"sourceTimeOfDay", time}, {"marketState", state}, {"timestamp", QJsonValue::Null}};
        if (!asks.isEmpty()) out.value.data.insert("ask", asks.first().toObject().value("price"));
        if (!bids.isEmpty()) out.value.data.insert("bid", bids.first().toObject().value("price"));
    } else if (op == Operation::TradingSchedule) {
        if (!raw.value("output").isArray()) return bad(); QJsonArray days;
        for (auto v : raw.value("output").toArray()) {
            const auto o = v.toObject(); const auto date = str(o, "bass_dt"), open = str(o, "opnd_yn");
            if (!digits(date, 8, 8) || !QDate::fromString(date, "yyyyMMdd").isValid() || (open != "Y" && open != "N")) return bad();
            days.append(QJsonObject{{"date", date}, {"open", open == "Y"}});
        }
        out.value.data = {{"days", days}};
    } else if (op == Operation::Reference) {
        if (!raw.value("output2").isArray()) return bad(); QDateTime latest;
        for (auto v : raw.value("output2").toArray()) {
            const auto o = v.toObject(); const auto d = str(o, "stck_bsop_date"), t = str(o, "stck_cntg_hour");
            if (!digits(d, 8, 8) || !digits(t, 6, 6)) return bad();
            const QDateTime stamp(QDate::fromString(d, "yyyyMMdd"), QTime::fromString(t, "HHmmss"), Seoul);
            if (!stamp.isValid()) return bad(); if (!latest.isValid() || stamp > latest) latest = stamp;
        }
        out.value.data = {{"symbol", step.input.symbol}, {"tradeDate", latest.isValid() ? latest.date().toString("yyyyMMdd") : QString()},
            {"minuteTimestamp", latest.isValid() ? QJsonValue(latest.toString(Qt::ISODateWithMs)) : QJsonValue(QJsonValue::Null)}};
        if (latest.isValid()) out.sourceTime = {SourceTimeQuality::FullTimestamp, latest.toString(Qt::ISODateWithMs), "stck_bsop_date+stck_cntg_hour", latest};
    } else if (op == Operation::OpenOrders || op == Operation::History || op == Operation::QueryOrder) {
        const bool possible = step.variant == "amendable-orders";
        const auto array = raw.value(possible ? "output" : "output1"); if (!array.isArray()) return bad();
        QJsonArray available;
        for (auto v : array.toArray()) {
            if (!v.isObject()) return bad(); const auto o = v.toObject();
            const auto date = possible ? (step.input.endTimeMs > 0 ? QDateTime::fromMSecsSinceEpoch(step.input.endTimeMs, Qt::UTC).toTimeZone(Seoul).date() : utcNow_().toTimeZone(Seoul).date()).toString("yyyyMMdd") : str(o, "ord_dt");
            const auto venue = str(o, "excg_id_dvsn_cd").isEmpty() ? str(o, "excg_id_dvsn_Cd") : str(o, "excg_id_dvsn_cd");
            OrderRecord row; row.orderId = makeRef(date, str(o, "ord_gno_brno"), str(o, "odno"), venue);
            row.symbol = str(o, "pdno"); row.currency = "KRW"; row.market = "domestic_equity";
            row.side = str(o, "sll_buy_dvsn_cd") == "02" ? "buy" : str(o, "sll_buy_dvsn_cd") == "01" ? "sell" : QString();
            const auto type = str(o, "ord_dvsn_cd"); row.type = type == "00" ? "limit" : type == "01" ? "market" : "unsupported";
            row.price = type == "01" ? QString() : str(o, "ord_unpr"); row.quantity = str(o, "ord_qty"); row.executed = str(o, "tot_ccld_qty");
            row.quoteAmount = str(o, "tot_ccld_amt"); row.remaining = possible ? str(o, "psbl_qty") : str(o, "rmn_qty");
            if (row.orderId.isEmpty() || !symbol(row.symbol) || row.side.isEmpty() || !digits(row.quantity) || !digits(row.executed) || !digits(row.remaining) ||
                !num(row.quoteAmount) || (!row.price.isEmpty() && !num(row.price)) || decimal::compare(row.executed, row.quantity) > 0 || decimal::compare(row.remaining, row.quantity) > 0) return bad();
            const auto original = str(o, "orgn_odno");
            const auto knownTarget = mutationTargets_.value(row.orderId);
            const auto target = ref(knownTarget);
            if (!target.date.isEmpty() && target.number == original && target.date == date && target.venue == venue)
                row.originalOrderId = knownTarget; // Never infer the parent's branch from the child's branch.
            if (possible) {
                row.status = positive(row.executed) ? "partial" : "open"; row.rawStatus = "amendable";
                available.append(QJsonObject{{"orderId", row.orderId}, {"symbol", row.symbol}, {"side", row.side}, {"type", row.type},
                    {"price", row.price}, {"quantity", row.quantity}, {"executed", row.executed}, {"possibleQuantity", row.remaining}});
            } else {
                const auto cancel = str(o, "cncl_cfrm_qty"), reject = str(o, "rjct_qty"), canceled = str(o, "cncl_yn");
                if (!digits(cancel) || !digits(reject) || (canceled != "Y" && canceled != "N")) return bad();
                row.rawStatus = "cancel=" + canceled + ";confirmed=" + cancel + ";rejected=" + reject;
                if (positive(row.remaining)) row.status = positive(row.executed) ? "partial" : "open";
                else if (decimal::compare(row.executed, row.quantity) == 0 && positive(row.quantity)) row.status = "filled";
                else if (positive(cancel) || canceled == "Y") row.status = "canceled";
                else if (positive(reject)) row.status = "rejected";
                else row.status = "unknown";
            }
            const bool supported = venue == "KRX" && (row.type == "limit" || row.type == "market") && positive(row.remaining);
            row.cancelable = supported;
            row.amendable = supported && row.type == "limit";
            if (op != Operation::QueryOrder || row.orderId == step.input.orderId) out.value.orders.append(row);
        }
        if (possible) out.value.data = {{"possibleOrders", available}};
        out.value.scope = QStringLiteral("국내 일반 현금 주문 · 예약·신용 제외 · 날짜/거래소 범위 표시");
    } else  return bad(QStringLiteral("이 한국투자증권 응답 동작은 구현되지 않았습니다."), false);

    const bool paged = op == Operation::Holdings || op == Operation::Account || op == Operation::OpenOrders || op == Operation::History || op == Operation::QueryOrder;
    if (paged) {
        const auto cont = header(http, "tr_cont");
        if (cont != "D" && cont != "E" && cont != "F" && cont != "M") return bad();
        if (cont == "F" || cont == "M") {
            QString fk = str(raw, "ctx_area_fk100"), nk = str(raw, "ctx_area_nk100");
            const auto cleanup = qScopeGuard([&] { wipe(fk); wipe(nk); });
            if (!validText(fk, 100) || !validText(nk, 100) || (fk.trimmed().isEmpty() && nk.trimmed().isEmpty()) || cursors_.size() >= 1000) return bad();
            Cursor cursor; cursor.operation = op; cursor.variant = step.variant; cursor.binding = binding(step);
            try { cursor.fk = SecureBuffer::fromUtf8(fk); cursor.nk = SecureBuffer::fromUtf8(nk); } catch (...) { return bad(); }
            const auto handle = QUuid::createUuid().toString(QUuid::WithoutBraces);
            cursors_.emplace(handle, std::move(cursor)); out.value.hasMore = true; out.value.nextCursor = handle;
        }
        if (!step.cursorHandle.isEmpty()) cursors_.erase(step.cursorHandle);
    }
    out.value.ok = true;
    out.accountReadVerified = (op == Operation::Holdings || op == Operation::Account) && !out.value.hasMore;
    if (op == Operation::QueryOrder && out.value.orders.isEmpty() && !out.value.hasMore) out.value.notFound = true;
    return out;
}

BrokerReadiness KisAdapter::actionReadiness(BrokerAction action, const OrderInput& input,
                                            const QList<BrokerEvidence>& evidence, const OrderLimits& limits) const {
    if (!sessionReady()) return denied(BrokerNotReady::NotAuthenticated, QStringLiteral("먼저 한국투자증권에 연결하세요."));
    if (!selected_) return denied(BrokerNotReady::AccountNotVerified, QStringLiteral("등록 계좌를 선택하고 조회하세요."));
    if (action == BrokerAction::Read) return {true, BrokerNotReady::None, {}};
    auto find = [&](Operation op, const QString& variant = QString()) -> const BrokerEvidence* {
        for (auto i = evidence.crbegin(); i != evidence.crend(); ++i)
            if (i->complete && i->operation == op && (variant.isEmpty() || i->variant == variant)) return &*i;
        return nullptr;
    };
    if (action == BrokerAction::Cancel || action == BrokerAction::Amend) {
        const auto* possible = find(Operation::OpenOrders, "amendable-orders");
        if (!possible) return denied(BrokerNotReady::MissingEvidence, QStringLiteral("정정·취소 가능 수량을 먼저 조회하세요."));
        bool matched = false;
        for (auto v : possible->data.value("possibleOrders").toArray()) {
            const auto row = v.toObject(); if (str(row, "orderId") != input.orderId) continue;
            const auto amount = str(row, "possibleQuantity");
            if (matched || str(row, "symbol") != input.symbol || str(row, "side") != input.side || !positive(amount) || ref(input.orderId).venue != "KRX")
                return denied(BrokerNotReady::RejectedBySafetyRule, QStringLiteral("원주문과 변경 가능 수량을 다시 확인하세요."));
            if (action == BrokerAction::Amend && (!positive(input.quantity) || decimal::compare(input.quantity, amount) > 0 ||
                str(row, "type") != "limit" || input.type != "limit" || !positive(str(row, "price")) || !positive(input.price) ||
                decimal::compare(str(row, "price"), input.price) == 0))
                return denied(BrokerNotReady::RejectedBySafetyRule, QStringLiteral("정정할 미체결 수량과 새로운 지정가를 확인하세요."));
            matched = true;
        }
        if (!matched) return denied(BrokerNotReady::MissingEvidence, QStringLiteral("이 원주문의 변경 가능 수량을 찾지 못했습니다."));
        if (action == BrokerAction::Cancel) return {true, BrokerNotReady::None, QStringLiteral("체결분을 제외한 미체결 잔량 전체 취소")};
    }
    const auto* rules = find(Operation::Rules); const auto* book = find(Operation::Depth);
    if (!rules || !book) return denied(BrokerNotReady::MissingEvidence, QStringLiteral("종목 조건과 호가를 다시 조회하세요."));
    if (rules->data.value("symbol").toString() != input.symbol || book->data.value("symbol").toString() != input.symbol)
        return denied(BrokerNotReady::ChangedInput, QStringLiteral("조회한 종목과 주문 종목이 다릅니다."));
    const auto now = utcNow_().toTimeZone(Seoul);
    const auto source = QTime::fromString(str(book->data, "sourceTimeOfDay"), "HHmmss");
    // Receipt TTL is independently checked by the controller. Do not synthesize
    // a source date: this compares time-of-day only, with separate day evidence.
    if (str(book->data, "marketState") != "20" || !source.isValid() || now.time() < QTime(9, 0) || now.time() >= QTime(15, 20) ||
        std::abs(source.msecsTo(now.time())) > 10000)
        return denied(BrokerNotReady::RejectedBySafetyRule, QStringLiteral("현재 KRX 정규장 연속매매와 최근 호가 시각을 확인할 수 없습니다."));
    {
        const auto* calendar = find(Operation::TradingSchedule); bool open = false;
        if (calendar) for (auto v : calendar->data.value("days").toArray()) {
            const auto day = v.toObject(); if (str(day, "date") == now.date().toString("yyyyMMdd") && day.value("open").isBool()) open = day.value("open").toBool();
        }
        if (!open) return denied(BrokerNotReady::MissingEvidence, QStringLiteral("공식 개장일을 확인하지 못했습니다."));
    }
    MarketSnapshot snapshot;
    for (const auto& e : evidence) if (e.complete) snapshot.data.insert(e.operation, e.data);
    snapshot.data[Operation::Rules] = rules->data; snapshot.data[Operation::Depth] = book->data;
    if (input.side == "buy") {
        const auto* market = find(Operation::BuyingPower, "cash-market-cap");
        const auto* exact = input.type == "limit" ? find(Operation::BuyingPower, "cash-exact") : market;
        if (!market || !exact) return denied(BrokerNotReady::MissingEvidence, QStringLiteral("주문 조건별 미수 없는 매수 한도를 조회하세요."));
        const auto mq = str(market->data, "noMarginQuantity"), eq = str(exact->data, "noMarginQuantity");
        const auto ma = str(market->data, "noMarginBuyingPower"), ea = str(exact->data, "noMarginBuyingPower");
        if (!digits(mq) || !digits(eq) || !num(ma) || !num(ea) || !positive(input.quantity) ||
            decimal::compare(input.quantity, mq) > 0 || decimal::compare(input.quantity, eq) > 0)
            return denied(BrokerNotReady::RejectedBySafetyRule, QStringLiteral("미수 없는 매수 가능 수량이 부족합니다."));
        auto power = exact->data;
        power.insert("noMarginBuyingPower", decimal::compare(ma, ea) < 0 ? ma : ea);
        power.insert("noMarginQuantity", decimal::compare(mq, eq) < 0 ? mq : eq);
        snapshot.data[Operation::BuyingPower] = power;
    }
    const auto validation = validate(input, snapshot, limits);
    if (!validation.ok) return denied(BrokerNotReady::RejectedBySafetyRule, validation.message);
    return {true, BrokerNotReady::None, QStringLiteral("수수료 추정 미확인 · 주문은 사용자 버튼으로만 전송"), validation};
}

OrderValidation KisAdapter::validate(const OrderInput& in, const MarketSnapshot& snapshot, const OrderLimits& limits) const {
    auto deny = [](const QString& message) { OrderValidation v; v.message = message; return v; };
    if (!symbol(in.symbol) || (in.side != "buy" && in.side != "sell") || (in.type != "limit" && in.type != "market") ||
        !digits(in.quantity, 1, 10) || !positive(in.quantity) || !in.quoteAmount.isEmpty()) return deny(QStringLiteral("국내 현금 주문의 종목·방향·유형·정수 수량을 확인하세요."));
    const auto rules = snapshot.data.value(Operation::Rules), book = snapshot.data.value(Operation::Depth);
    if (str(rules, "symbol") != in.symbol || str(book, "symbol") != in.symbol || !rules.value("catalogTradable").toBool() ||
        !QStringList{"KOSPI", "KOSDAQ"}.contains(str(rules, "catalogMarket")) || str(rules, "temp_stop_yn") != "N" ||
        str(rules, "sltr_yn") != "N" || str(rules, "short_over_yn") != "N" || str(rules, "statusCode") == "58")
        return deny(QStringLiteral("이 종목의 KRX 일반 현금 주문 조건을 확인할 수 없습니다."));
    const auto last = str(rules, "lastPrice"), low = str(rules, "lowerLimitPrice"), high = str(rules, "upperLimitPrice"), group = str(rules, "securityType");
    const auto quantityStep = str(rules, "quantityStep");
    if (!positive(last) || !positive(low) || !positive(high) || !positive(quantityStep) || !decimal::multipleOf(in.quantity, quantityStep) ||
        tick(last, group).isEmpty() || tick(last, group) != str(rules, "tickSize")) return deny(QStringLiteral("현재 공식 호가·수량 단위와 지원 규칙이 일치하지 않습니다."));
    QString adverse = in.price, maxValue = in.price;
    if (in.type == "limit") {
        if (!digits(in.price) || !positive(in.price) || !decimal::multipleOf(in.price, tick(in.price, group))) return deny(QStringLiteral("지정가 호가 단위를 확인하세요."));
    } else {
        if (!in.price.isEmpty()) return deny(QStringLiteral("시장가에는 지정가를 입력하지 마세요."));
        auto remaining = in.quantity; QString previous;
        for (auto v : book.value(in.side == "buy" ? "asks" : "bids").toArray()) {
            const auto row = v.toObject(); const auto price = str(row, "price"), quantity = str(row, "quantity");
            if (!positive(price) || !digits(quantity) || !positive(quantity)) return deny(QStringLiteral("시장가 검증에 필요한 호가가 올바르지 않습니다."));
            if (!previous.isEmpty() && ((in.side == "buy" && decimal::compare(price, previous) < 0) || (in.side == "sell" && decimal::compare(price, previous) > 0)))
                return deny(QStringLiteral("호가 순서가 올바르지 않습니다."));
            previous = price; adverse = price;
            if (maxValue.isEmpty() || decimal::compare(price, maxValue) > 0) maxValue = price;
            if (decimal::compare(quantity, remaining) >= 0) { remaining = "0"; break; }
            remaining = decimal::subtract(remaining, quantity);
        }
        if (positive(remaining) || !positive(adverse)) return deny(QStringLiteral("시장가 주문 수량을 검증할 호가 잔량이 부족합니다."));
    }
    if (decimal::compare(low, high) > 0 || decimal::compare(adverse, low) < 0 || decimal::compare(maxValue, high) > 0)
        return deny(QStringLiteral("당일 상·하한가 범위를 확인하세요."));
    const auto notional = decimal::multiply(maxValue, in.quantity);
    if (limits.currency != "KRW" || !positive(limits.maxNotional) || !num(limits.maxDeviationPercent) || decimal::compare(notional, limits.maxNotional) > 0)
        return deny(QStringLiteral("원화 주문 최대 금액을 설정하거나 수량을 줄이세요."));
    const auto delta = decimal::compare(adverse, last) >= 0 ? decimal::subtract(adverse, last) : decimal::subtract(last, adverse);
    if (decimal::compare(decimal::multiply(delta, "100"), decimal::multiply(last, limits.maxDeviationPercent)) > 0)
        return deny(QStringLiteral("설정한 현재가 대비 괴리율을 초과했습니다."));
    if (in.side == "buy") {
        const auto p = snapshot.data.value(Operation::BuyingPower); const auto amount = str(p, "noMarginBuyingPower"), qty = str(p, "noMarginQuantity");
        if (str(p, "symbol") != in.symbol || !num(amount) || !digits(qty) || decimal::compare(notional, amount) > 0 || decimal::compare(in.quantity, qty) > 0)
            return deny(QStringLiteral("이번 주문의 미수 없는 매수 한도가 부족하거나 확인되지 않았습니다."));
    } else {
        QString cashQuantity = "0";
        for (const auto& v : snapshot.data.value(Operation::Holdings).value("items").toArray()) {
            const auto row = v.toObject();
            if (str(row, "symbol") == in.symbol && row.value("cashHolding").toBool() && digits(str(row, "sellableQuantity")))
                cashQuantity = decimal::add(cashQuantity, str(row, "sellableQuantity"));
        }
        if (decimal::compare(in.quantity, cashQuantity) > 0) return deny(QStringLiteral("확인된 현금 보유 주식의 주문 가능 수량이 부족합니다."));
        const auto p = snapshot.data.value(Operation::SellableQuantity); const auto qty = str(p, "sellableQuantity");
        if (str(p, "symbol") != in.symbol || !digits(qty) || decimal::compare(in.quantity, qty) > 0)
            return deny(QStringLiteral("이번 주문의 매도 가능 수량이 부족하거나 확인되지 않았습니다."));
    }
    OrderValidation result; result.ok = true; result.notional = notional; result.currency = "KRW"; return result;
}
QString KisAdapter::amendDescription() const { return QStringLiteral("KRX 원주문의 미체결 수량을 새로운 지정가로 정정합니다. 부분 정정 시 나머지는 원주문에 남습니다."); }
BrokerReconciliation KisAdapter::reconcile(const QString& kind, const OrderInput& intent, const QList<OrderRecord>& observed) const {
    BrokerReconciliation result; result.message = QStringLiteral("정확한 서버 주문 참조를 확인할 때까지 재전송하지 않습니다.");
    const auto operationId = intent.operationOrderId.isEmpty() && kind == "place" ? intent.orderId : intent.operationOrderId;
    if (!orderReferenceValid(operationId)) return result;
    const OrderRecord* operation = nullptr; const OrderRecord* original = nullptr;
    for (const auto& r : observed) {
        if (r.orderId == operationId) { if (operation) return result; operation = &r; }
        if (r.orderId == intent.orderId) { if (original) return result; original = &r; }
    }
    auto identity = [&](const OrderRecord& r) { return r.market == "domestic_equity" && r.currency == "KRW" && r.symbol == intent.symbol && r.side == intent.side; };
    auto terms = [&](const OrderRecord& r) {
        return identity(r) && r.type == intent.type && num(r.quantity) && num(intent.quantity) && decimal::compare(r.quantity, intent.quantity) == 0 &&
            (intent.type == "market" ? r.price.isEmpty() && intent.price.isEmpty() : positive(r.price) && positive(intent.price) && decimal::compare(r.price, intent.price) == 0);
    };
    if (!operation || !identity(*operation) || operation->status == "unknown") return result;
    if (kind == "place") {
        if (!terms(*operation)) return result;
        result.resolved = true; result.applied = operation->status != "rejected" || positive(operation->executed);
    } else {
        if (!original || operationId == intent.orderId || operation->originalOrderId != intent.orderId || !identity(*original) || original->status == "unknown") return result;
        if (operation->status == "rejected") { result.resolved = true; result.applied = false; }
        else if (kind == "cancel" && original->status == "canceled" && num(original->remaining) &&
                 decimal::compare(original->remaining, "0") == 0 && operation->status == "canceled") {
            result.resolved = true; result.applied = true;
        } else if (kind == "amend" && terms(*operation) && QStringList{"open", "partial", "filled", "canceled"}.contains(operation->status)) {
            result.resolved = true; result.applied = true;
        }
    }
    if (result.resolved) result.message = result.applied ? QStringLiteral("주문일자·지점·번호·거래소와 주문 내용을 대조했습니다.") : QStringLiteral("정확한 작업 주문 참조에서 거부 상태를 확인했습니다.");
    return result;
}
} // namespace keefi
