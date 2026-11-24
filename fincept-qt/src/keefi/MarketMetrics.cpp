#include "keefi/MarketMetrics.h"
#include "keefi/TradingTransport.h"
#include <algorithm>
#include <QDateTime>
#include <QPointer>
#include <QRegularExpression>
#include <QJsonArray>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
namespace keefi {
namespace {
bool validSymbol(const QString& symbol) {
    return QRegularExpression("\\A[A-Z0-9]{1,20}USDT\\z").match(symbol).hasMatch();
}
bool validPublishedIdentity(const QString& symbol) {
    // The complete public adjustment list includes non-ASCII contracts even
    // when KEEFI's selected contract is restricted to ASCII. Preserve identity.
    return !symbol.isEmpty() && symbol.size() <= 128 && symbol.isValidUtf16() &&
        std::none_of(symbol.cbegin(), symbol.cend(), [](QChar c) {
            return c.isSpace() || c.category() == QChar::Other_Control;
        });
}
QString percentDifference(const QString& a, const QString& b) {
    if (!decimal::positive(a) || !decimal::positive(b)) return {};
    const bool negative = decimal::compare(a, b) < 0;
    QString numerator = decimal::multiply(negative ? decimal::subtract(b, a) : decimal::subtract(a, b), "100");
    QString denominator = b;
    const auto scale = [](const QString& n) { const auto at = n.indexOf('.'); return at < 0 ? 0 : n.size() - at - 1; };
    const auto places = qMax(scale(numerator), scale(denominator));
    numerator += QString(places - scale(numerator), '0'); numerator.remove('.');
    denominator += QString(places - scale(denominator), '0'); denominator.remove('.');
    numerator += QString(6, '0');
    if (!decimal::valid(numerator) || !decimal::positive(denominator)) return {};
    QString quotient, remainder = "0";
    for (const auto digit : numerator) {
        remainder = decimal::normalize(remainder + digit);
        int n = 0;
        while (decimal::compare(remainder, denominator) >= 0) {
            remainder = decimal::subtract(remainder, denominator); ++n;
        }
        quotient += QChar('0' + n);
    }
    quotient = quotient.rightJustified(7, '0');
    quotient.insert(quotient.size() - 6, '.');
    quotient = decimal::normalize(quotient);
    return negative && quotient != "0" ? "-" + quotient : quotient;
}
bool fresh(const AssetEvidence& e, qint64 now) {
    return e.complete && !e.source.isEmpty() && !e.url.isEmpty() &&
        e.sourceTimeMs > 0 && e.sourceTimeMs <= now && now - e.sourceTimeMs <= MarketMetrics::FreshnessMs &&
        e.receivedAtMs > 0 && e.receivedAtMs <= now && now - e.receivedAtMs <= MarketMetrics::FreshnessMs;
}
}
struct MarketMetrics::Private {
    QPointer<TradingTransport> transport;
    PublicDerivativesMetrics data;
    QList<Read> queue;
    QTimer refresh, pacing;
    quint64 token = 0, nextToken = 1;
    Read activeRead = Read::Contracts;
    ContractObservation previous;
    void next() {
        if (!transport || token || pacing.isActive() || queue.isEmpty() || data.symbol.isEmpty()) return;
        activeRead = queue.takeFirst(); token = nextToken++;
        pacing.start(); // One public request per second, including rapid selection changes.
        transport->send(token, MarketMetrics::request(activeRead, data.symbol));
    }
    void enqueue() {
        if (data.symbol.isEmpty() || token || !queue.isEmpty()) return;
        queue = {Read::Contracts, Read::FundingIntervals, Read::Funding, Read::OpenInterest}; next();
    }
};
MarketMetrics::MarketMetrics(TradingTransport* transport, QObject* p) : QObject(p), d_(std::make_unique<Private>()) {
    d_->transport = transport; d_->refresh.setInterval(60000);
    d_->pacing.setInterval(1000); d_->pacing.setSingleShot(true);
    connect(&d_->refresh, &QTimer::timeout, this, [this] { d_->enqueue(); });
    connect(&d_->pacing, &QTimer::timeout, this, [this] { d_->next(); });
    if (transport) connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& response) {
        if (!token || token != d_->token) return;
        d_->token = 0;
        const auto read = d_->activeRead;
        const bool ok = applyResponse(read, d_->data.symbol, response, QDateTime::currentMSecsSinceEpoch(), &d_->data);
        if (read == Read::Contracts && !ok) d_->queue.clear();
        if(!ok&&read!=Read::FundingIntervals)d_->previous={};
        if(read==Read::OpenInterest){
            const ContractObservation current{d_->data.symbol,d_->data.markPrice,d_->data.openInterest,
                d_->data.fundingEvidence.sourceTimeMs,d_->data.openInterestEvidence.sourceTimeMs};
            const auto flow=contractFlow(d_->previous,current,QDateTime::currentMSecsSinceEpoch());
            d_->data.flowLabel=flow.label;d_->data.flowReason=flow.reason;d_->data.flowFromMs=flow.fromMs;d_->data.flowToMs=flow.toMs;
            d_->data.flowAvailable=flow.available;d_->data.flowPriceDirection=flow.priceDirection;d_->data.flowQuantityDirection=flow.quantityDirection;
            const auto now=QDateTime::currentMSecsSinceEpoch();
            d_->previous=decimal::positive(current.price)&&!decimal::normalize(current.quantity).isEmpty()&&
                fresh(d_->data.fundingEvidence,now)&&fresh(d_->data.openInterestEvidence,now)&&
                qAbs(current.priceAtMs-current.quantityAtMs)<=MaximumLegSkewMs?current:ContractObservation{};
        }
        emit changed(); d_->next();
    });
}
MarketMetrics::~MarketMetrics() { setAsset({}); }
ContractFlow MarketMetrics::contractFlow(const ContractObservation& before,const ContractObservation& after,qint64 now){
    ContractFlow out;out.reason=QStringLiteral("같은 구간의 가격·미결제약정 관측 2회 필요");
    if(before.symbol!=after.symbol||!validSymbol(before.symbol))return out;
    for(const auto& s:{before,after}){
        if(!decimal::positive(s.price)||decimal::normalize(s.quantity).isEmpty()||s.priceAtMs<=0||s.quantityAtMs<=0||
            s.priceAtMs>now||s.quantityAtMs>now||now-s.priceAtMs>FreshnessMs||now-s.quantityAtMs>FreshnessMs||
            qAbs(s.priceAtMs-s.quantityAtMs)>MaximumLegSkewMs)return out;
    }
    const auto gap=after.quantityAtMs-before.quantityAtMs;
    if(gap<=0||gap>120000||after.priceAtMs<=before.priceAtMs)return out;
    const auto directionSign=[](int value){return (value>0)-(value<0);};
    out.available=true;out.reason.clear();out.priceDirection=directionSign(decimal::compare(after.price,before.price));
    out.quantityDirection=directionSign(decimal::compare(after.quantity,before.quantity));out.fromMs=before.quantityAtMs;out.toMs=after.quantityAtMs;
    const auto direction=[](int n){return n>0?QStringLiteral("상승"):n<0?QStringLiteral("하락"):QStringLiteral("보합");};
    out.label=QStringLiteral("가격 %1 · 미결제약정 %2").arg(direction(out.priceDirection),direction(out.quantityDirection));return out;
}
void MarketMetrics::setAsset(const QString& asset) {
    const auto symbol = validSymbol(asset + "USDT") && !asset.isEmpty() ? asset + "USDT" : QString{};
    if (d_->data.symbol == symbol) return;
    const bool inFlight = d_->token != 0; d_->token = 0;
    if (inFlight && d_->transport) d_->transport->abort();
    d_->queue.clear(); d_->data = {}; d_->data.symbol = symbol;d_->previous={};
    d_->data.fundingReason = d_->data.intervalReason = d_->data.openInterestReason = pendingReason();
    if (symbol.isEmpty()) d_->refresh.stop(); else { d_->refresh.start(); d_->enqueue(); }
    emit changed();
}
PublicDerivativesMetrics MarketMetrics::derivatives(qint64 now) const {
    if (!now) now = QDateTime::currentMSecsSinceEpoch();
    auto data = d_->data;
    if (!fresh(data.fundingEvidence, now) || data.nextFundingTimeMs <= now) {
        data.fundingRate.clear(); data.interestRate.clear(); data.nextFundingTimeMs = 0;data.markPrice.clear();
        if (data.fundingReason.isEmpty()) data.fundingReason = "최신 펀딩 자료 부족";
    }
    if (!fresh(data.openInterestEvidence, now)) {
        data.openInterest.clear(); data.openInterestUnit.clear();
        if (data.openInterestReason.isEmpty()) data.openInterestReason = "최신 미결제약정 자료 부족";
    }
    if(data.markPrice.isEmpty()||data.openInterest.isEmpty()||(data.flowAvailable&&
        (data.flowFromMs<=0||data.flowToMs<=data.flowFromMs||data.flowToMs>now||now-data.flowFromMs>FreshnessMs))){
        data.flowLabel.clear();data.flowReason="최근 가격·미결제약정 자료 부족";data.flowFromMs=data.flowToMs=0;
        data.flowAvailable=false;data.flowPriceDirection=data.flowQuantityDirection=0;
    }
    const auto intervalAt = data.intervalEvidence.receivedAtMs;
    if (!data.intervalEvidence.complete || intervalAt <= 0 || intervalAt > now || now - intervalAt > FreshnessMs) {
        data.fundingIntervalHours = 0; data.fundingIntervalDefault = false;
        if (data.intervalReason.isEmpty()) data.intervalReason = "최신 정산 주기 자료 부족";
    }
    return data;
}
ExchangePriceGap MarketMetrics::exchangePriceGap(const AssetQuote& upbit, const AssetQuote& binance, const AssetQuote& bridge, qint64 now) {
    ExchangePriceGap out; out.asOfMs = now; out.legs = {upbit, binance, bridge};
    if(upbit.quoteAsset=="USD")out.label=QStringLiteral("코인베이스 프리미엄 · USD/USDT 환산");
    if (upbit.baseAsset.isEmpty() || upbit.baseAsset != binance.baseAsset || (upbit.quoteAsset != "KRW"&&upbit.quoteAsset!="USD") ||
        binance.quoteAsset != "USDT" || bridge.baseAsset != "USDT" || bridge.quoteAsset != upbit.quoteAsset) {
        out.reason = "자산 또는 환산 단위 불일치"; return out;
    }
    qint64 earliest = now, latest = 0;
    for (const auto& leg : out.legs) {
        if (!decimal::positive(leg.price) || !fresh(leg.evidence, now)) { out.reason = "최신 원천 시세 부족"; return out; }
        earliest = qMin(earliest, leg.evidence.sourceTimeMs); latest = qMax(latest, leg.evidence.sourceTimeMs);
    }
    if (latest - earliest > MaximumLegSkewMs) { out.reason = "원천 시각 차이 30초 초과"; return out; }
    out.percent = priceGapPercent(upbit.price, binance.price, bridge.price);
    out.available = !out.percent.isEmpty();
    if (!out.available) out.reason = "계산 가능한 소수 범위 초과";
    return out;
}
QString MarketMetrics::priceGapPercent(const QString& localPrice, const QString& remotePrice, const QString& bridgePrice) {
    return percentDifference(localPrice, decimal::multiply(remotePrice, bridgePrice));
}
RequestSpec MarketMetrics::request(Read read, const QString& symbol) {
    RequestSpec r; r.operation = Operation::Markets;
    QString path;
    switch (read) {
    case Read::Contracts: path = "/fapi/v1/exchangeInfo"; break;
    case Read::FundingIntervals: path = "/fapi/v1/fundingInfo"; break;
    case Read::Funding: path = "/fapi/v1/premiumIndex"; break;
    case Read::OpenInterest: path = "/fapi/v1/openInterest"; break;
    default: return r;
    }
    if (read != Read::Contracts && read != Read::FundingIntervals && !validSymbol(symbol)) return r;
    r.url = QUrl("https://fapi.binance.com" + path);
    if (read == Read::Funding || read == Read::OpenInterest) {
        QUrlQuery q; q.addQueryItem("symbol", symbol); r.url.setQuery(q);
    }
    r.headers.insert("Accept", "application/json");
    return r;
}
bool MarketMetrics::isRequest(const RequestSpec& r) {
    if (r.method != "GET" || r.authenticated || !r.body.isEmpty() || r.operation != Operation::Markets) return false;
    const auto symbol = QUrlQuery(r.url).queryItemValue("symbol");
    for (const auto read : {Read::Contracts, Read::FundingIntervals, Read::Funding, Read::OpenInterest}) {
        const auto expected = request(read, symbol);
        if (expected.url.isValid() && r.url == expected.url && r.headers == expected.headers) return true;
    }
    return false;
}
bool MarketMetrics::applyResponse(Read read, const QString& requestedSymbol, const HttpResult& response,
                                  qint64 now, PublicDerivativesMetrics* result) {
    const QString symbol = requestedSymbol; // The caller may pass result->symbol itself.
    if (!result || !validSymbol(symbol) || now <= 0) return false;
    auto& m = *result;
    const auto previousContract = read == Read::Contracts ? m : PublicDerivativesMetrics{};
    if (m.symbol != symbol) { m = {}; m.symbol = symbol; }
    const QString unavailable = response.status == 429 ? "원천 요청 제한" : "원천 자료 미수신 또는 검증 실패";
    switch (read) {
    case Read::Contracts: m = {}; m.symbol = symbol; m.fundingReason = m.intervalReason = m.openInterestReason = "지원 계약 미확인"; break;
    case Read::Funding: m.fundingRate.clear(); m.interestRate.clear(); m.nextFundingTimeMs = 0; m.fundingEvidence = {}; m.fundingReason = unavailable;m.markPrice.clear(); break;
    case Read::FundingIntervals: m.fundingIntervalHours = 0; m.fundingIntervalDefault = false; m.intervalEvidence = {}; m.intervalReason = unavailable; break;
    case Read::OpenInterest: m.openInterest.clear(); m.openInterestUnit.clear(); m.openInterestEvidence = {}; m.openInterestReason = unavailable; break;
    }
    if (response.status != 200 || response.timedOut || response.networkError || response.localRetryAfterSeconds > 0) return false;
    const auto document = parseExactJson(response.body, nullptr, read == Read::Contracts ? JsonResponseScope::BinanceMarketCatalog : JsonResponseScope::Standard);
    if (document.isNull()) return false;
    const auto evidence = [&](qint64 at) { return AssetEvidence{"Binance 공식 USDⓈ-M", request(read, symbol).url.toString(), "public:binance:usdm:" + symbol, at, now, true}; };
    const auto timestamp = [&](const QJsonValue& v) { bool ok = false; const auto at = v.toString().toLongLong(&ok); return ok && at > 0 && at <= now && now - at <= FreshnessMs ? at : qint64(0); };
    const auto signedDecimal = [](QString value) { const bool negative = value.startsWith('-'); if (negative) value.remove(0, 1); value = decimal::normalize(value); return value.isEmpty() ? QString{} : negative && value != "0" ? "-" + value : value; };
    if (read == Read::Contracts) {
        if (!document.isObject() || !document.object().value("symbols").isArray()) return false;
        QString base; int matches = 0;
        for (const auto& v : document.object().value("symbols").toArray()) {
            const auto o = v.toObject();
            if (o.value("symbol").toString() != symbol) continue;
            ++matches;
            if (o.value("contractType").toString() != "PERPETUAL" || o.value("quoteAsset").toString() != "USDT" ||
                o.value("marginAsset").toString() != "USDT" || o.value("status").toString() != "TRADING" ||
                o.value("underlyingType").toString() != "COIN") return false;
            base = o.value("baseAsset").toString();
            if (!QRegularExpression("\\A[A-Z0-9]{1,20}\\z").match(base).hasMatch() || base + "USDT" != symbol) return false;
        }
        if (matches != 1) return false;
        // Reconfirming the same contract does not publish new funding/OI data.
        // Keep their original evidence; derivatives() still rejects expired legs.
        if (previousContract.symbol == symbol && previousContract.baseAsset == base) m = previousContract;
        m.baseAsset = base; return true;
    }
    if (m.baseAsset.isEmpty()) return false;
    if (read == Read::FundingIntervals) {
        if (!document.isArray() || !response.body.isValidUtf8()) return false;
        QSet<QString> seen; int interval = 8; bool found = false;
        for (const auto& v : document.array()) {
            const auto o = v.toObject(); const auto id = o.value("symbol").toString();
            bool ok = false; const auto hours = o.value("fundingIntervalHours").toString().toInt(&ok);
            if (!validPublishedIdentity(id) || seen.contains(id) || !ok || hours < 1 || hours > 24) return false;
            seen.insert(id);
            if (id == symbol) { interval = hours; found = true; }
        }
        // Official FAQ default applies only after a successful complete adjustment list.
        m.fundingIntervalHours = interval; m.fundingIntervalDefault = !found;
        m.intervalEvidence = evidence(0); m.intervalReason.clear(); return true;
    }
    if (!document.isObject()) return false;
    const auto o = document.object();
    if (o.value("symbol").toString() != symbol) return false;
    const auto at = timestamp(o.value("time"));
    if (!at) return false;
    if (read == Read::Funding) {
        const auto rate = signedDecimal(o.value("lastFundingRate").toString());
        const auto interest = signedDecimal(o.value("interestRate").toString());
        bool ok = false; const auto next = o.value("nextFundingTime").toString().toLongLong(&ok);
        if (rate.isEmpty() || interest.isEmpty() || !ok || next <= now || next - now > 24 * 3600000LL) return false;
        m.fundingRate = rate; m.interestRate = interest; m.nextFundingTimeMs = next;
        const auto mark=decimal::normalize(o.value("markPrice").toString());m.markPrice=decimal::positive(mark)?mark:QString{};
        m.fundingEvidence = evidence(at); m.fundingReason.clear(); return true;
    }
    if (read == Read::OpenInterest) {
        const auto amount = decimal::normalize(o.value("openInterest").toString());
        if (amount.isEmpty()) return false;
        m.openInterest = amount; m.openInterestUnit = "수량";
        m.openInterestEvidence = evidence(at); m.openInterestReason.clear(); return true;
    }
    return false;
}
} // namespace keefi
