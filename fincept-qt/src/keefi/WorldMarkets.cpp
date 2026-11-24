#include "keefi/WorldMarkets.h"
#include "keefi/TradingRequestLimits.h"
#include "keefi/TradingTransport.h"
#include "keefi/XmlInputGuard.h"
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QTimeZone>
#include <QTimer>
#include <QXmlStreamReader>
#include <cmath>
#include <optional>

namespace keefi {
namespace {
struct Spec {
    WorldMarketItem item;
    const char* name;
    const char* code;
    const char* category; // nullptr: an index read from the index "basic" page.
};
constexpr std::array<Spec, WorldMarketItemCount> Specs{{
    {WorldMarketItem::Spx, "S&P 500", ".INX", nullptr},
    {WorldMarketItem::Ndx, "나스닥 100", ".NDX", nullptr},
    {WorldMarketItem::Kospi, "코스피", "KOSPI", nullptr},
    {WorldMarketItem::Nikkei, "닛케이 225", ".N225", nullptr},
    {WorldMarketItem::HangSeng, "항셍", ".HSI", nullptr},
    {WorldMarketItem::Ftse, "FTSE 100", ".FTSE", nullptr},
    {WorldMarketItem::Gold, "금", "GCcv1", "metals"},
    {WorldMarketItem::Silver, "은", "SIcv1", "metals"},
    {WorldMarketItem::Wti, "WTI 원유", "CLcv1", "energy"},
    {WorldMarketItem::UsdKrw, "원/달러", "FX_USDKRW", "exchange"},
    {WorldMarketItem::JpyKrw, "원/100엔", "FX_JPYKRW", "exchange"},
    {WorldMarketItem::CnyKrw, "원/위안", "FX_CNYKRW", "exchange"},
    {WorldMarketItem::AudKrw, "원/호주달러", "FX_AUDKRW", "exchange"},
}};
constexpr bool specsFollowTheEnum() {
    for (int i = 0; i < WorldMarketItemCount; ++i) if (int(Specs[i].item) != i) return false;
    return true;
}
static_assert(specsFollowTheEnum(), "Specs must be indexed by WorldMarketItem");
// Bank postings first: the Today tether premium waits on 원/달러.
constexpr std::array<WorldMarketItem, WorldMarketItemCount> Priority{
    WorldMarketItem::UsdKrw, WorldMarketItem::JpyKrw, WorldMarketItem::AudKrw, WorldMarketItem::CnyKrw,
    WorldMarketItem::Spx, WorldMarketItem::Ndx, WorldMarketItem::Kospi, WorldMarketItem::Nikkei,
    WorldMarketItem::HangSeng, WorldMarketItem::Ftse, WorldMarketItem::Gold, WorldMarketItem::Silver, WorldMarketItem::Wti};

const QString EcbNamespace = QStringLiteral("http://www.ecb.int/vocabulary/2002-08-01/eurofxref");

QString text(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    return value.isString() ? value.toString() : QString{};
}
// Provider decimals may be grouped ("7,688.62") and signed ("-1,110.73").
QString plainSigned(const QString& raw) {
    static const QRegularExpression pattern(QStringLiteral("\\A([+-]?)((?:[0-9]{1,3}(?:,[0-9]{3})+)|[0-9]{1,15})(\\.[0-9]{1,10})?\\z"));
    const auto match = pattern.match(raw);
    if (!match.hasMatch()) return {};
    auto digits = match.captured(2);
    digits.remove(QLatin1Char(','));
    return (match.captured(1) == QStringLiteral("-") ? QStringLiteral("-") : QString{}) + digits + match.captured(3);
}
std::optional<double> number(const QString& plain) {
    bool ok = false;
    const double value = plain.toDouble(&ok);
    if (plain.isEmpty() || !ok || !std::isfinite(value)) return std::nullopt;
    return value;
}
QString signedFixed(double value, int decimals) {
    auto result = QString::number(value, 'f', decimals);
    if (result.startsWith(QLatin1Char('-')) && QString(result).remove(QLatin1Char('-')).remove(QLatin1Char('0')).remove(QLatin1Char('.')).isEmpty())
        result.remove(0, 1); // "-0.00" is flat, not a fall.
    return result;
}
// The provider's own clock string, e.g. 2026-11-04T16:51:11-05:00. Its date part is
// the venue-local trading date; never re-derive it from the receipt time.
qint64 isoTime(const QString& value, QDate* localDate) {
    static const QRegularExpression pattern(QStringLiteral("\\A([0-9]{4}-[0-9]{2}-[0-9]{2})T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:Z|[+-][0-9]{2}:[0-9]{2})\\z"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) return 0;
    const auto at = QDateTime::fromString(value, Qt::ISODate);
    const auto date = QDate::fromString(match.captured(1), Qt::ISODate);
    if (!at.isValid() || !date.isValid()) return 0;
    if (localDate) *localDate = date;
    return at.toMSecsSinceEpoch();
}
bool limited(const HttpResult& response) {
    return response.status == 429 || response.status == 418 || response.localRetryAfterSeconds > 0;
}
bool received(const HttpResult& response) {
    return response.status == 200 && !response.networkError && !response.timedOut && !limited(response);
}
qint64 retryDelay(const HttpResult& response) {
    return qBound(WorldMarkets::MinimumRetryMs, TradingRequestLimits::retryAfterMs(response), WorldMarkets::MaximumRetryMs);
}
QString failureText(const QString& name, const HttpResult& response, const QString& reason) {
    if (limited(response)) return name + QStringLiteral(" 요청 제한 · 재시도 대기");
    return reason.isEmpty() ? name + QStringLiteral(" 수신 실패 또는 검증 실패") : reason;
}
// ICE U.S. Dollar Index weights over ECB reference rates (units of currency per EUR).
std::optional<double> dollarIndexFrom(const QHash<QString, QString>& rates) {
    struct Leg { const char* code; double low, high; };
    static constexpr std::array<Leg, 6> legs{{{"USD", 0.5, 3}, {"JPY", 50, 500}, {"GBP", 0.3, 2}, {"CAD", 0.5, 3}, {"SEK", 3, 30}, {"CHF", 0.3, 3}}};
    std::array<double, 6> value{};
    for (int i = 0; i < 6; ++i) {
        const auto raw = rates.value(QLatin1String(legs[i].code));
        if (raw.size() > 24 || !decimal::positive(raw)) return std::nullopt;
        const auto parsed = number(raw);
        if (!parsed || *parsed < legs[i].low || *parsed > legs[i].high) return std::nullopt;
        value[i] = *parsed;
    }
    const double usd = value[0];
    const double result = 50.14348112 * std::pow(usd, -0.576) * std::pow(value[1] / usd, 0.136) *
                          std::pow(usd / value[2], -0.119) * std::pow(value[3] / usd, 0.091) *
                          std::pow(value[4] / usd, 0.042) * std::pow(value[5] / usd, 0.036);
    if (!std::isfinite(result) || result < 40 || result > 250) return std::nullopt;
    return result;
}
} // namespace

WorldMarketsDemand WorldMarketsDemand::full() {
    WorldMarketsDemand demand;
    for (const auto item : Priority) demand.quotes.append(item);
    demand.dollarIndex = demand.japanYield = true;
    return demand;
}
WorldMarketsDemand WorldMarketsDemand::chartHeader() {
    WorldMarketsDemand demand;
    demand.quotes = {WorldMarketItem::UsdKrw, WorldMarketItem::Spx, WorldMarketItem::Ndx, WorldMarketItem::Kospi};
    demand.dollarIndex = true;
    return demand;
}

struct WorldMarkets::Private {
    struct Lane { QPointer<TradingTransport> transport; quint64 token = 0; int item = -1; qint64 blockedUntil = 0; };
    WorldMarkets* q = nullptr;
    WorldMarketsDemand demand;
    Lane naver, ecb, mof;
    std::array<qint64, WorldMarketItemCount> due{};
    qint64 ecbDue = 0, mofDue = 0;
    Clock clock;
    QTimer timer;
    bool active = false, pumpQueued = false;
    quint64 nextToken = 1;
    WorldMarketsSnapshot data;

    // A finished request frees its lane; the next due read goes out on the next turn
    // of the event loop, never re-entrantly from inside the transport's signal.
    void pump() {
        if (pumpQueued) return;
        pumpQueued = true;
        QTimer::singleShot(0, q, [this] { pumpQueued = false; q->refresh(); });
    }
    void naverFinished(quint64 token, const HttpResult& response) {
        if (!active || !token || naver.token != token) return;
        const int index = naver.item;
        naver.token = 0; naver.item = -1;
        if (index < 0 || index >= WorldMarketItemCount) return;
        const auto now = clock();
        const auto item = WorldMarketItem(index);
        WorldMarketQuote next; QString reason;
        bool ok = received(response) && WorldMarkets::parseQuote(item, response.body, now, &next, &reason);
        auto& held = data.quotes[index];
        if (ok && held.available() && next.tradedAtMs < held.tradedAtMs) {
            ok = false; reason = WorldMarkets::itemName(item) + QStringLiteral(" 기준 시각이 이전 응답보다 과거예요");
        }
        if (ok && next.session == WorldMarketSession::PreOpen && held.available() && held.session == WorldMarketSession::Closed && held.price == next.price) {
            // Before the open the provider repeats the last close with a zero change. Keep
            // that session's real close and change; only record that it is still current.
            held.receivedAtMs = now; data.failures[index].clear(); due[index] = now + RefreshMs;
        } else if (ok) {
            held = next; data.failures[index].clear();
            const bool closedIndex = !Specs[index].category && next.session == WorldMarketSession::Closed;
            due[index] = now + (closedIndex ? ClosedRefreshMs : RefreshMs);
        } else {
            const auto wait = retryDelay(response);
            data.failures[index] = failureText(WorldMarkets::itemName(item), response, reason);
            due[index] = now + wait;
            if (limited(response)) naver.blockedUntil = now + wait; // One limit pauses the whole host lane.
        }
        emit q->changed();
        pump();
    }
    void ecbFinished(quint64 token, const HttpResult& response) {
        if (!active || !token || ecb.token != token) return;
        ecb.token = 0;
        const auto now = clock();
        WorldDollarIndex next; QString reason;
        bool ok = received(response) && WorldMarkets::parseEcbDollarIndex(response.body, now, &next, &reason);
        if (ok && data.dollarIndex.available() && next.date < data.dollarIndex.date) {
            ok = false; reason = QStringLiteral("달러 인덱스 ECB 기준일이 이전 응답보다 과거예요");
        }
        if (ok) { data.dollarIndex = next; data.dollarIndexFailure.clear(); ecbDue = now + DailyRefreshMs; }
        else { data.dollarIndexFailure = failureText(QStringLiteral("달러 인덱스"), response, reason); ecbDue = now + retryDelay(response); }
        emit q->changed();
        pump();
    }
    void mofFinished(quint64 token, const HttpResult& response) {
        if (!active || !token || mof.token != token) return;
        mof.token = 0;
        const auto now = clock();
        WorldJapanYield next; QString reason;
        bool ok = received(response) && WorldMarkets::parseJapanYield(response.body, now, &next, &reason);
        if (ok && data.japanYield.available() && next.date < data.japanYield.date) {
            ok = false; reason = QStringLiteral("일본 10년물 기준일이 이전 응답보다 과거예요");
        }
        if (ok) { data.japanYield = next; data.japanYieldFailure.clear(); mofDue = now + DailyRefreshMs; }
        else { data.japanYieldFailure = failureText(QStringLiteral("일본 10년물"), response, reason); mofDue = now + retryDelay(response); }
        emit q->changed();
        pump();
    }
};

WorldMarkets::WorldMarkets(TradingTransport* naver, TradingTransport* ecb, TradingTransport* mof, QObject* parent, Clock clock)
    : QObject(parent), d_(std::make_unique<Private>()) {
    d_->q = this;
    d_->clock = clock ? std::move(clock) : Clock([] { return QDateTime::currentMSecsSinceEpoch(); });
    d_->naver.transport = naver; d_->ecb.transport = ecb; d_->mof.transport = mof;
    if (naver) connect(naver, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& r) { d_->naverFinished(token, r); });
    if (ecb) connect(ecb, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& r) { d_->ecbFinished(token, r); });
    if (mof) connect(mof, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& r) { d_->mofFinished(token, r); });
    d_->timer.setInterval(1000);
    connect(&d_->timer, &QTimer::timeout, this, &WorldMarkets::refresh);
}
WorldMarkets::~WorldMarkets() { setActive(false); }

void WorldMarkets::setActive(bool active) { setDemand(active ? WorldMarketsDemand::full() : WorldMarketsDemand{}); }
bool WorldMarkets::active() const { return d_->active; }
WorldMarketsDemand WorldMarkets::demand() const { return d_->demand; }
void WorldMarkets::setDemand(const WorldMarketsDemand& demand) {
    if (d_->demand == demand) return;
    d_->demand = demand;
    d_->active = demand.any();
    // A read that is no longer wanted is cancelled; its item stays due, so it is read
    // again as soon as something asks for it.
    const auto cancel = [](Private::Lane& lane) {
        const bool inFlight = lane.token != 0;
        lane.token = 0; lane.item = -1;
        if (inFlight && lane.transport) lane.transport->abort();
    };
    if (!d_->naver.token || d_->naver.item < 0 || !demand.wants(WorldMarketItem(d_->naver.item))) cancel(d_->naver);
    if (!demand.dollarIndex) cancel(d_->ecb);
    if (!demand.japanYield) cancel(d_->mof);
    if (d_->active) { if (!d_->timer.isActive()) d_->timer.start(); refresh(); }
    else d_->timer.stop();
    emit changed();
}

void WorldMarkets::refresh() {
    if (!d_->active) return;
    const auto now = d_->clock();
    const auto send = [this](Private::Lane& lane, int item, const RequestSpec& spec) {
        lane.token = d_->nextToken++; lane.item = item;
        lane.transport->send(lane.token, spec);
    };
    if (d_->naver.transport && !d_->naver.token && now >= d_->naver.blockedUntil)
        for (const auto item : Priority)
            if (d_->demand.wants(item) && d_->due[int(item)] <= now) { send(d_->naver, int(item), request(item)); break; }
    if (d_->demand.dollarIndex && d_->ecb.transport && !d_->ecb.token && now >= d_->ecbDue) send(d_->ecb, 0, ecbRequest());
    if (d_->demand.japanYield && d_->mof.transport && !d_->mof.token && now >= d_->mofDue) send(d_->mof, 0, mofRequest());
}

WorldMarketsSnapshot WorldMarkets::snapshot() const {
    auto result = d_->data;
    result.active = d_->active;
    return result;
}

QString WorldMarkets::itemName(WorldMarketItem item) {
    const auto index = int(item);
    return index >= 0 && index < WorldMarketItemCount ? QString::fromUtf8(Specs[index].name) : QString{};
}

RequestSpec WorldMarkets::request(WorldMarketItem item) {
    RequestSpec r;
    const auto index = int(item);
    if (index < 0 || index >= WorldMarketItemCount) return r;
    const auto& spec = Specs[index];
    r.operation = Operation::Markets;
    if (item == WorldMarketItem::Kospi) r.url = QUrl(QStringLiteral("https://m.stock.naver.com/api/index/KOSPI/basic"));
    else if (!spec.category) r.url = QUrl(QStringLiteral("https://api.stock.naver.com/index/%1/basic").arg(QLatin1String(spec.code)));
    else r.url = QUrl(QStringLiteral("https://m.stock.naver.com/front-api/marketIndex/productDetail?category=%1&reutersCode=%2")
                          .arg(QLatin1String(spec.category), QLatin1String(spec.code)));
    r.headers.insert("Accept", "application/json");
    return r;
}
RequestSpec WorldMarkets::ecbRequest() {
    RequestSpec r;
    r.operation = Operation::Markets;
    r.url = QUrl(QStringLiteral("https://www.ecb.europa.eu/stats/eurofxref/eurofxref-hist-90d.xml"));
    r.headers.insert("Accept", "application/xml");
    return r;
}
RequestSpec WorldMarkets::mofRequest() {
    RequestSpec r;
    r.operation = Operation::Markets;
    r.url = QUrl(QStringLiteral("https://www.mof.go.jp/english/policy/jgbs/reference/interest_rate/jgbcme.csv"));
    r.headers.insert("Accept", "text/csv");
    return r;
}
bool WorldMarkets::isRequest(const RequestSpec& r) {
    if (r.operation != Operation::Markets || r.method != "GET" || r.authenticated || !r.body.isEmpty()) return false;
    const auto same = [&r](const RequestSpec& expected) { return r.url == expected.url && r.headers == expected.headers; };
    for (int i = 0; i < WorldMarketItemCount; ++i) if (same(request(WorldMarketItem(i)))) return true;
    return same(ecbRequest()) || same(mofRequest());
}

bool WorldMarkets::parseQuote(WorldMarketItem item, const QByteArray& body, qint64 now, WorldMarketQuote* out, QString* reason) {
    const auto name = itemName(item);
    const auto fail = [&](const QString& why) { if (reason) *reason = name + QLatin1Char(' ') + why; return false; };
    const auto index = int(item);
    if (!out || now <= 0 || index < 0 || index >= WorldMarketItemCount) return fail(QStringLiteral("확인 필요"));
    if (body.isEmpty() || body.size() > MaximumNaverBytes) return fail(QStringLiteral("응답 크기 확인 필요"));
    const auto& spec = Specs[index];
    const auto document = parseExactJson(body);
    if (!document.isObject()) return fail(QStringLiteral("응답 형식 확인 필요"));
    auto row = document.object();
    if (!spec.category) {
        if (text(row, "stockEndType") != QLatin1String("index") || text(row, "reutersCode") != QLatin1String(spec.code))
            return fail(QStringLiteral("지수 식별 확인 필요"));
    } else {
        if (row.value(QLatin1String("isSuccess")) != QJsonValue(true)) return fail(QStringLiteral("응답 실패"));
        row = row.value(QLatin1String("result")).toObject();
        if (text(row, "reutersCode") != QLatin1String(spec.code) || text(row, "categoryType") != QLatin1String(spec.category))
            return fail(QStringLiteral("종목 식별 확인 필요"));
    }
    const auto price = plainSigned(text(row, "closePrice"));
    const auto percent = plainSigned(text(row, "fluctuationsRatio"));
    const auto change = plainSigned(text(row, spec.category ? "fluctuations" : "compareToPreviousClosePrice"));
    const auto priceValue = number(price), percentValue = number(percent), changeValue = number(change);
    if (price.startsWith(QLatin1Char('-')) || !priceValue || *priceValue <= 0 || *priceValue >= 1e9) return fail(QStringLiteral("가격 확인 필요"));
    if (!percentValue || !changeValue || std::abs(*percentValue) > 60) return fail(QStringLiteral("등락 확인 필요"));
    // A row whose change and percentage disagree in direction is not shown.
    if ((*percentValue > 0 && *changeValue < 0) || (*percentValue < 0 && *changeValue > 0)) return fail(QStringLiteral("등락 부호 확인 필요"));
    QDate localDate;
    const auto tradedAt = isoTime(text(row, "localTradedAt"), &localDate);
    if (tradedAt <= 0 || tradedAt > now + 600000 || now - tradedAt > 45LL * 86400000) return fail(QStringLiteral("기준 시각 확인 필요"));
    const auto exchange = row.value(QLatin1String("stockExchangeType")).toObject();
    auto delayText = text(row, "delayTime");
    if (delayText.isEmpty()) delayText = text(exchange, "delayTime");
    bool delayOk = delayText.isEmpty();
    const int delay = delayText.isEmpty() ? 0 : delayText.toInt(&delayOk);
    if (!delayOk || delay < 0 || delay > 240) return fail(QStringLiteral("지연 시간 확인 필요"));
    const auto status = text(row, "marketStatus");
    auto venue = text(exchange, "nameKor").simplified();
    if (venue.size() > 40) venue.clear();
    WorldMarketQuote quote;
    quote.price = price; quote.change = change; quote.changePercent = percent;
    quote.session = status == QLatin1String("OPEN") ? WorldMarketSession::Open
                  : status == QLatin1String("CLOSE") ? WorldMarketSession::Closed
                  : status == QLatin1String("PREOPEN") ? WorldMarketSession::PreOpen : WorldMarketSession::Unknown;
    quote.tradedAtMs = tradedAt; quote.localDate = localDate; quote.delayMinutes = delay;
    quote.venue = venue; quote.receivedAtMs = now;
    *out = quote;
    if (reason) reason->clear();
    return true;
}

bool WorldMarkets::parseEcbDollarIndex(const QByteArray& body, qint64 now, WorldDollarIndex* out, QString* reason) {
    const auto fail = [&](const QString& why) { if (reason) *reason = QStringLiteral("달러 인덱스 ") + why; return false; };
    if (!out || now <= 0 || body.isEmpty() || body.size() > MaximumEcbBytes || !body.isValidUtf8()) return fail(QStringLiteral("ECB 응답 형식 확인 필요"));
    static const QStringList needed{QStringLiteral("USD"), QStringLiteral("JPY"), QStringLiteral("GBP"), QStringLiteral("CAD"), QStringLiteral("SEK"), QStringLiteral("CHF")};
    if (!xmlAttributeBudgetAllows(body)) return fail(QStringLiteral("ECB XML 형식 확인 필요"));
    QXmlStreamReader xml(body);
    QMap<QDate, QHash<QString, QString>> days;
    QDate current; bool dated = false; int depth = 0, dateDepth = -1;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) return fail(QStringLiteral("ECB XML 형식 확인 필요"));
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            if (xml.name() != QStringLiteral("Cube")) continue;
            if (xml.namespaceUri() != EcbNamespace) return fail(QStringLiteral("ECB 원천 형식 확인 필요"));
            const auto attributes = xml.attributes();
            if (attributes.hasAttribute(QStringLiteral("time"))) {
                if (dated) return fail(QStringLiteral("ECB 날짜 구조 확인 필요"));
                const auto date = QDate::fromString(attributes.value(QStringLiteral("time")).toString(), Qt::ISODate);
                if (!date.isValid() || days.contains(date)) return fail(QStringLiteral("ECB 기준일 확인 필요"));
                days.insert(date, {}); current = date; dated = true; dateDepth = depth;
            } else if (attributes.hasAttribute(QStringLiteral("currency"))) {
                if (!dated) return fail(QStringLiteral("ECB 기준일 누락"));
                const auto currency = attributes.value(QStringLiteral("currency")).toString();
                if (!needed.contains(currency)) continue;
                auto& rates = days[current];
                if (rates.contains(currency)) return fail(QStringLiteral("ECB 통화 중복"));
                rates.insert(currency, attributes.value(QStringLiteral("rate")).toString());
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (dated && depth == dateDepth) { dated = false; dateDepth = -1; }
            --depth;
        }
    }
    if (xml.hasError()) return fail(QStringLiteral("ECB XML 형식 확인 필요"));
    QList<QPair<QDate, double>> values; // Newest complete business days first.
    for (auto it = days.constEnd(); it != days.constBegin() && values.size() < 2;) {
        --it;
        if (const auto value = dollarIndexFrom(it.value())) values.append({it.key(), *value});
    }
    if (values.isEmpty()) return fail(QStringLiteral("계산 자료 확인 필요"));
    const auto today = QDateTime::fromMSecsSinceEpoch(now, QTimeZone("Europe/Berlin")).date();
    const auto age = values.first().first.daysTo(today);
    if (age < 0 || age > MaximumDailyAgeDays) return fail(QStringLiteral("ECB 기준일 확인 필요"));
    WorldDollarIndex result;
    result.value = QString::number(values.first().second, 'f', 2);
    result.date = values.first().first;
    result.receivedAtMs = now;
    if (values.size() > 1) {
        result.previousValue = QString::number(values.at(1).second, 'f', 2);
        result.previousDate = values.at(1).first;
        result.changePercent = signedFixed((values.first().second / values.at(1).second - 1) * 100, 2);
    }
    *out = result;
    if (reason) reason->clear();
    return true;
}

bool WorldMarkets::parseJapanYield(const QByteArray& body, qint64 now, WorldJapanYield* out, QString* reason) {
    const auto fail = [&](const QString& why) { if (reason) *reason = QStringLiteral("일본 10년물 ") + why; return false; };
    if (!out || now <= 0 || body.isEmpty() || body.size() > MaximumMofBytes) return fail(QStringLiteral("응답 크기 확인 필요"));
    static const QRegularExpression datePattern(QStringLiteral("\\A([0-9]{4})/([0-9]{1,2})/([0-9]{1,2})\\z"));
    static const QRegularExpression valuePattern(QStringLiteral("\\A-?[0-9]{1,2}(?:\\.[0-9]{1,4})?\\z"));
    int column = -1;
    QList<QPair<QDate, QString>> rows;
    // The table is ASCII; the trailing note line may be in a Japanese code page and is ignored.
    for (auto line : body.split('\n')) {
        line = line.trimmed();
        if (line.startsWith("Date,")) {
            if (column >= 0) return fail(QStringLiteral("머리글 확인 필요"));
            column = int(line.split(',').indexOf(QByteArray("10Y")));
            if (column <= 0) return fail(QStringLiteral("10년 열 확인 필요"));
            continue;
        }
        if (column < 0) continue;
        const auto cells = line.split(',');
        const auto match = datePattern.match(QString::fromLatin1(cells.value(0)));
        if (!match.hasMatch()) continue; // The blank separator and the note.
        const QDate date(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
        if (!date.isValid() || cells.size() <= column) return fail(QStringLiteral("표 형식 확인 필요"));
        const auto value = QString::fromLatin1(cells.at(column)).trimmed();
        if (value == QStringLiteral("-")) continue;
        if (!valuePattern.match(value).hasMatch()) return fail(QStringLiteral("금리 값 확인 필요"));
        if (!rows.isEmpty() && date <= rows.last().first) return fail(QStringLiteral("날짜 순서 확인 필요"));
        rows.append({date, value});
    }
    if (column < 0) return fail(QStringLiteral("표 형식 확인 필요"));
    if (rows.isEmpty()) return fail(QStringLiteral("이번 달 첫 공시 전"));
    const auto today = QDateTime::fromMSecsSinceEpoch(now, QTimeZone("Asia/Tokyo")).date();
    const auto age = rows.last().first.daysTo(today);
    if (age < 0 || age > MaximumDailyAgeDays) return fail(QStringLiteral("기준일 확인 필요"));
    WorldJapanYield result;
    result.tenYear = rows.last().second;
    result.date = rows.last().first;
    result.receivedAtMs = now;
    if (rows.size() > 1) { result.previousTenYear = rows.at(rows.size() - 2).second; result.previousDate = rows.at(rows.size() - 2).first; }
    *out = result;
    if (reason) reason->clear();
    return true;
}

WorldMarketQuote WorldMarkets::cross(const WorldMarketQuote& base, int baseUnit, const WorldMarketQuote& quote, int quoteUnit) {
    if (!base.available() || !quote.available() || baseUnit <= 0 || quoteUnit <= 0) return {};
    const auto b = number(base.price), q = number(quote.price), bc = number(base.change), qc = number(quote.change);
    if (!b || !q || !bc || !qc || *b <= 0 || *q <= 0) return {};
    const double value = (*b / baseUnit) / (*q / quoteUnit);
    const double previousBase = *b - *bc, previousQuote = *q - *qc;
    if (!std::isfinite(value) || previousBase <= 0 || previousQuote <= 0) return {};
    const double previous = (previousBase / baseUnit) / (previousQuote / quoteUnit);
    if (!std::isfinite(previous) || previous <= 0) return {};
    WorldMarketQuote out;
    out.price = QString::number(value, 'f', 2);
    out.change = signedFixed(value - previous, 2);
    out.changePercent = signedFixed((value / previous - 1) * 100, 2);
    const bool baseOlder = base.tradedAtMs <= quote.tradedAtMs;
    out.tradedAtMs = baseOlder ? base.tradedAtMs : quote.tradedAtMs; // The older leg is the honest basis.
    out.localDate = baseOlder ? base.localDate : quote.localDate;
    out.session = base.session == WorldMarketSession::Open && quote.session == WorldMarketSession::Open ? WorldMarketSession::Open
                : base.session == WorldMarketSession::Closed || quote.session == WorldMarketSession::Closed ? WorldMarketSession::Closed
                : WorldMarketSession::Unknown;
    out.delayMinutes = qMax(base.delayMinutes, quote.delayMinutes);
    out.venue = base.venue == quote.venue ? base.venue : QString{};
    out.receivedAtMs = qMin(base.receivedAtMs, quote.receivedAtMs);
    return out;
}

QString WorldMarkets::premiumPercent(const QString& usdtKrw, const QString& usdKrw) {
    if (usdtKrw.size() > 40 || usdKrw.size() > 40 || !decimal::positive(usdtKrw) || !decimal::positive(usdKrw)) return {};
    const auto tether = number(usdtKrw), dollar = number(usdKrw);
    if (!tether || !dollar || *dollar <= 0) return {};
    const double value = (*tether / *dollar - 1) * 100;
    if (!std::isfinite(value) || std::abs(value) > 50) return {};
    return signedFixed(value, 2);
}

QString WorldMarkets::spreadPoints(const QString& a, const QString& b) {
    static const QRegularExpression pattern(QStringLiteral("\\A-?[0-9]{1,3}(?:\\.[0-9]{1,6})?\\z"));
    if (!pattern.match(a).hasMatch() || !pattern.match(b).hasMatch()) return {};
    const auto left = number(a), right = number(b);
    if (!left || !right) return {};
    const double value = *left - *right;
    if (!std::isfinite(value) || std::abs(value) > 50) return {};
    auto result = signedFixed(value, 3);
    while (result.endsWith(QLatin1Char('0')) && result.size() - result.indexOf(QLatin1Char('.')) > 3) result.chop(1);
    return result;
}
} // namespace keefi
