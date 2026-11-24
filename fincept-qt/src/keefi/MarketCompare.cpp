#include "keefi/MarketCompare.h"

#include "keefi/ExchangeIdentifiers.h"
#include "keefi/MarketCandles.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>

namespace keefi {
namespace {
constexpr qint64 Minute = 60000, Hour = 3600000, Day = 86400000;

bool integerValue(const QJsonValue& value, qint64* output) {
    static const QRegularExpression exact("\\A[0-9]{1,16}\\z");
    const auto text = value.toString();
    if (!exact.match(text).hasMatch()) return false;
    bool ok = false;
    const auto number = text.toLongLong(&ok);
    if (!ok) return false;
    *output = number;
    return true;
}

bool positiveDecimal(const QString& value) {
    static const QRegularExpression exact("\\A[0-9]{1,40}(?:\\.[0-9]{1,40})?\\z");
    return exact.match(value).hasMatch() && decimal::positive(value);
}

QString host(CompareVenue venue) {
    return venue == CompareVenue::BinanceUsdM ? QStringLiteral("fapi.binance.com") : QStringLiteral("api.binance.com");
}

QString path(CompareVenue venue) {
    return venue == CompareVenue::BinanceUsdM ? QStringLiteral("/fapi/v1/klines") : QStringLiteral("/api/v3/klines");
}

CompareSpec perp(const char* name, const char* symbol, const char* type) {
    CompareSpec spec;
    spec.name = QString::fromUtf8(name);
    spec.symbol = QString::fromLatin1(symbol);
    spec.identity = QStringLiteral("perp:binance-usdm:") + spec.symbol;
    spec.venue = CompareVenue::BinanceUsdM;
    spec.underlyingType = QString::fromLatin1(type);
    spec.currency = QStringLiteral("USDT");
    return spec;
}

CompareSpec spot(const char* name, const char* symbol) {
    CompareSpec spec;
    spec.name = QString::fromUtf8(name);
    spec.symbol = QString::fromLatin1(symbol);
    spec.identity = QStringLiteral("coin:binance:") + spec.symbol;
    spec.venue = CompareVenue::BinanceSpot;
    spec.currency = QStringLiteral("USDT");
    return spec;
}

QList<CompareSpec> allowList() {
    return MarketCompare::defaultSpecs() + MarketCompare::optionalSpecs();
}

std::optional<CompareSpec> allowedSpec(CompareVenue venue, const QString& symbol) {
    for (const auto& spec : allowList())
        if (spec.venue == venue && spec.symbol == symbol) return spec;
    return std::nullopt;
}
} // namespace

QList<CompareSpec> MarketCompare::defaultSpecs() {
    return {perp("금", "XAUUSDT", "COMMODITY"), perp("WTI 원유", "CLUSDT", "COMMODITY"),
            perp("나스닥 100", "QQQUSDT", "EQUITY"), spot("비트코인", "BTCUSDT")};
}

QList<CompareSpec> MarketCompare::optionalSpecs() {
    return {perp("은", "XAGUSDT", "COMMODITY"), perp("브렌트유", "BZUSDT", "COMMODITY"),
            perp("구리", "COPPERUSDT", "COMMODITY"), perp("S&P 500", "SPYUSDT", "EQUITY"),
            perp("러셀 2000", "IWMUSDT", "EQUITY"), perp("한국", "EWYUSDT", "EQUITY"),
            perp("일본", "EWJUSDT", "EQUITY"), perp("코스피 200", "KODEX200USDT", "KR_EQUITY")};
}

CompareSpec MarketCompare::upbitBitcoin() {
    CompareSpec spec;
    spec.name = QStringLiteral("비트코인(원화)");
    spec.symbol = QStringLiteral("KRW-BTC");
    spec.identity = QStringLiteral("coin:upbit:KRW-BTC");
    spec.venue = CompareVenue::UpbitKrw;
    spec.currency = QStringLiteral("KRW");
    return spec;
}

bool MarketCompare::supportedInterval(qint64 intervalMs) { return !wireInterval(intervalMs).isEmpty(); }

QString MarketCompare::wireInterval(qint64 intervalMs) {
    switch (intervalMs) {
    case Minute: return QStringLiteral("1m");
    case 5 * Minute: return QStringLiteral("5m");
    case 15 * Minute: return QStringLiteral("15m");
    case Hour: return QStringLiteral("1h");
    case 4 * Hour: return QStringLiteral("4h");
    case Day: return QStringLiteral("1d");
    default: return {};
    }
}

RequestSpec MarketCompare::klinesRequest(const CompareSpec& spec, qint64 intervalMs, int limit) {
    RequestSpec request;
    request.operation = Operation::Markets;
    if (!supportedInterval(intervalMs) || limit < 1 || limit > 1000) return request;
    if (spec.venue == CompareVenue::UpbitKrw) {
        if (limit != DefaultLimit || spec != upbitBitcoin()) return request;
        return MarketCandles::request(spec.identity, intervalMs);
    }
    if (!allowedSpec(spec.venue, spec.symbol)) return request;
    QUrl url(QStringLiteral("https://") + host(spec.venue) + path(spec.venue));
    QUrlQuery query;
    query.addQueryItem("symbol", spec.symbol);
    query.addQueryItem("interval", wireInterval(intervalMs));
    query.addQueryItem("limit", QString::number(limit));
    url.setQuery(query);
    request.url = url;
    request.headers.insert("Accept", "application/json");
    return request;
}

RequestSpec MarketCompare::perpCatalogRequest() {
    RequestSpec request;
    request.operation = Operation::Markets;
    request.url = QUrl(QStringLiteral("https://fapi.binance.com/fapi/v1/exchangeInfo"));
    request.headers.insert("Accept", "application/json");
    return request;
}

bool MarketCompare::isRequest(const RequestSpec& request) {
    if (request.method != "GET" || request.authenticated || !request.body.isEmpty() || request.operation != Operation::Markets)
        return false;
    const auto& url = request.url;
    if (url.scheme() != "https" || url.port(443) != 443 || !url.userInfo().isEmpty() || url.hasFragment()) return false;
    const auto same = [&](const RequestSpec& expected) {
        return !expected.url.isEmpty() && request.url == expected.url && request.headers == expected.headers;
    };
    if (same(perpCatalogRequest())) return true;
    const QUrlQuery query(url);
    const auto items = query.queryItems();
    if (items.size() != 3 || items[0].first != "symbol" || items[1].first != "interval" || items[2].first != "limit") return false;
    CompareVenue venue;
    if (url.host() == "api.binance.com" && url.path() == "/api/v3/klines") venue = CompareVenue::BinanceSpot;
    else if (url.host() == "fapi.binance.com" && url.path() == "/fapi/v1/klines") venue = CompareVenue::BinanceUsdM;
    else return false;
    const auto spec = allowedSpec(venue, items[0].second);
    if (!spec) return false;
    qint64 interval = 0;
    for (qint64 candidate : {Minute, 5 * Minute, 15 * Minute, Hour, 4 * Hour, Day})
        if (wireInterval(candidate) == items[1].second) interval = candidate;
    bool ok = false;
    const int limit = items[2].second.toInt(&ok);
    if (!interval || !ok || QString::number(limit) != items[2].second) return false;
    return same(klinesRequest(*spec, interval, limit));
}

CompareSeries MarketCompare::parseKlines(const CompareSpec& spec, qint64 intervalMs, const HttpResult& response, qint64 receivedAtMs) {
    CompareSeries out;
    out.spec = spec;
    out.intervalMs = intervalMs;
    out.receivedAtMs = receivedAtMs;
    if (response.status == 429 || response.status == 418) { out.reason = QStringLiteral("원천 요청 제한"); return out; }
    if (response.status != 200 || response.networkError || response.timedOut || receivedAtMs <= 0 || !supportedInterval(intervalMs)) {
        out.reason = QStringLiteral("봉 응답을 받지 못했어요");
        return out;
    }
    if (spec.venue == CompareVenue::UpbitKrw) {
        const auto series = MarketCandles::parse(spec.identity, intervalMs, response, receivedAtMs, receivedAtMs);
        if (!series.available) { out.reason = series.reason.isEmpty() ? QStringLiteral("업비트 봉 형식 확인 필요") : series.reason; return out; }
        for (const auto& candle : series.candles) {
            CompareBar bar;
            bar.openMs = candle.startsAtMs;
            bar.closeMs = candle.startsAtMs + intervalMs - 1;
            bar.closeText = candle.close;
            bar.close = candle.close.toDouble();
            if (bar.closeMs >= receivedAtMs || !(bar.close > 0)) continue;
            out.bars.append(bar);
        }
        out.available = !out.bars.isEmpty();
        if (!out.available) out.reason = QStringLiteral("완료된 봉이 없어요");
        return out;
    }
    QString error;
    const auto doc = parseExactJson(response.body, &error);
    if (!doc.isArray() || doc.array().size() > 1000) { out.reason = QStringLiteral("봉 응답 형식 확인 필요"); return out; }
    QList<CompareBar> bars;
    qint64 previous = -1;
    for (const auto& value : doc.array()) {
        if (!value.isArray()) { out.reason = QStringLiteral("봉 응답 형식 확인 필요"); return out; }
        const auto tuple = value.toArray();
        CompareBar bar;
        if (tuple.size() != 12 || !integerValue(tuple[0], &bar.openMs) || !integerValue(tuple[6], &bar.closeMs)) {
            out.reason = QStringLiteral("봉 응답 형식 확인 필요");
            return out;
        }
        const auto open = tuple[1].toString(), high = tuple[2].toString(), low = tuple[3].toString(), close = tuple[4].toString();
        if (!positiveDecimal(open) || !positiveDecimal(high) || !positiveDecimal(low) || !positiveDecimal(close) ||
            bar.closeMs != bar.openMs + intervalMs - 1 || bar.openMs % intervalMs != 0 || bar.openMs <= previous ||
            decimal::compare(high, low) < 0 || decimal::compare(high, close) < 0 || decimal::compare(close, low) < 0) {
            out.reason = QStringLiteral("봉 값이나 시각이 맞지 않아요");
            return out;
        }
        previous = bar.openMs;
        if (bar.closeMs >= receivedAtMs) continue; // Forming bar: drawn elsewhere, never calculated.
        bar.closeText = decimal::normalize(close);
        bar.close = bar.closeText.toDouble();
        bars.append(bar);
    }
    out.bars = bars;
    out.available = !bars.isEmpty();
    if (!out.available) out.reason = QStringLiteral("완료된 봉이 없어요");
    return out;
}

QHash<QString, QString> MarketCompare::parsePerpCatalog(const HttpResult& response, QString* reason) {
    QHash<QString, QString> out;
    const auto fail = [&](const QString& why) { if (reason) *reason = why; return QHash<QString, QString>{}; };
    if (response.status != 200 || response.networkError || response.timedOut) return fail(QStringLiteral("선물 종목 목록을 받지 못했어요"));
    const auto doc = parseExactJson(response.body, nullptr, JsonResponseScope::BinanceMarketCatalog);
    if (!doc.isObject() || !doc.object().value("symbols").isArray()) return fail(QStringLiteral("선물 종목 목록 형식 확인 필요"));
    for (const auto& value : doc.object().value("symbols").toArray()) {
        if (!value.isObject()) return fail(QStringLiteral("선물 종목 목록 형식 확인 필요"));
        const auto row = value.toObject();
        const auto symbol = row.value("symbol").toString();
        // Crypto perpetuals say PERPETUAL; commodity/equity contracts say TRADIFI_PERPETUAL.
        const auto contract = row.value("contractType").toString();
        if ((contract != "PERPETUAL" && contract != "TRADIFI_PERPETUAL") || row.value("status").toString() != "TRADING") continue;
        if (!binanceIdentifierValid(symbol, 2)) continue;
        out.insert(symbol, row.value("underlyingType").toString());
    }
    if (out.isEmpty()) return fail(QStringLiteral("거래 중인 무기한 계약이 없어요"));
    if (reason) reason->clear();
    return out;
}

bool MarketCompare::perpAllowed(const CompareSpec& spec, const QHash<QString, QString>& catalog, QString* reason) {
    const auto fail = [&](const QString& why) { if (reason) *reason = why; return false; };
    if (spec.venue != CompareVenue::BinanceUsdM) { if (reason) reason->clear(); return true; }
    const auto expected = allowedSpec(spec.venue, spec.symbol);
    if (!expected || *expected != spec) return fail(QStringLiteral("허용 목록에 없는 계약이에요"));
    if (!catalog.contains(spec.symbol)) return fail(QStringLiteral("거래 중인 무기한 계약이 아니에요"));
    if (catalog.value(spec.symbol) != spec.underlyingType)
        return fail(QStringLiteral("거래소 분류(%1)가 허용 목록(%2)과 달라요").arg(catalog.value(spec.symbol), spec.underlyingType));
    if (reason) reason->clear();
    return true;
}

QList<ComparePercentPoint> MarketCompare::percentFromBase(const CompareSeries& series, qint64 baseOpenMs) {
    QList<ComparePercentPoint> out;
    const auto base = std::find_if(series.bars.cbegin(), series.bars.cend(), [&](const auto& bar) { return bar.openMs == baseOpenMs; });
    if (base == series.bars.cend() || !(base->close > 0)) return out;
    for (auto it = base; it != series.bars.cend(); ++it) out.append({it->openMs, (it->close / base->close - 1.0) * 100.0});
    return out;
}

std::optional<double> MarketCompare::pearson(const QList<std::pair<double, double>>& pairs) {
    if (pairs.size() < 2) return std::nullopt;
    double meanA = 0, meanB = 0;
    for (const auto& [a, b] : pairs) { meanA += a; meanB += b; }
    meanA /= pairs.size();
    meanB /= pairs.size();
    double cov = 0, varA = 0, varB = 0;
    for (const auto& [a, b] : pairs) {
        cov += (a - meanA) * (b - meanB);
        varA += (a - meanA) * (a - meanA);
        varB += (b - meanB) * (b - meanB);
    }
    if (varA <= 1e-24 || varB <= 1e-24) return std::nullopt;
    const auto r = cov / std::sqrt(varA * varB);
    if (!std::isfinite(r)) return std::nullopt;
    return std::clamp(r, -1.0, 1.0);
}

QList<CorrelationPoint> MarketCompare::rollingCorrelation(const CompareSeries& base, const CompareSeries& other) {
    QList<CorrelationPoint> out;
    if (base.intervalMs <= 0 || base.intervalMs != other.intervalMs) return out;
    QHash<qint64, CompareBar> others;
    for (const auto& bar : other.bars) others.insert(bar.openMs, bar);
    const auto paired = [&](const CompareBar& bar) {
        const auto found = others.constFind(bar.openMs);
        return found != others.cend() && found->closeMs == bar.closeMs && found->close > 0 && bar.close > 0;
    };
    QList<std::pair<double, double>> run;
    for (int i = 0; i < base.bars.size(); ++i) {
        const auto& bar = base.bars[i];
        CorrelationPoint point;
        point.openMs = bar.openMs;
        const bool hasPrevious = i > 0 && base.bars[i - 1].openMs == bar.openMs - base.intervalMs;
        if (hasPrevious && paired(bar) && paired(base.bars[i - 1])) {
            const auto& previous = base.bars[i - 1];
            const auto& otherNow = others[bar.openMs];
            const auto& otherBefore = others[previous.openMs];
            run.append({std::log(bar.close / previous.close), std::log(otherNow.close / otherBefore.close)});
        } else {
            run.clear();
        }
        point.pairs20 = std::min<int>(20, run.size());
        point.pairs60 = std::min<int>(60, run.size());
        if (run.size() >= 20) point.r20 = pearson(run.mid(run.size() - 20));
        if (run.size() >= 60) point.r60 = pearson(run.mid(run.size() - 60));
        out.append(point);
    }
    return out;
}

QList<RelationChange> MarketCompare::relationChanges(const QList<CorrelationPoint>& points) {
    QList<RelationChange> out;
    // -1: a correlation is missing, 0: no condition, 1: condition.
    const auto state = [&](int index) {
        const auto& point = points[index];
        if (!point.r20 || !point.r60) return -1;
        return std::abs(*point.r20) >= RelationMinimumAbs && std::abs(*point.r60) >= RelationMinimumAbs &&
                       ((*point.r20 > 0) != (*point.r60 > 0))
                   ? 1
                   : 0;
    };
    constexpr int span = RelationPersistBars + RelationClearBars;
    for (int i = span - 1; i < points.size(); ++i) {
        bool mark = true;
        for (int k = 0; k < RelationPersistBars && mark; ++k) mark = state(i - k) == 1;
        for (int k = RelationPersistBars; k < span && mark; ++k) mark = state(i - k) == 0;
        if (mark) out.append({points[i].openMs, *points[i].r20, *points[i].r60});
    }
    return out;
}

QString MarketCompare::relationLabel(std::optional<double> r) {
    if (!r) return QStringLiteral("표본 부족");
    if (*r >= TogetherThreshold) return QStringLiteral("같이 움직임");
    if (*r <= -TogetherThreshold) return QStringLiteral("반대로 움직임");
    return QStringLiteral("따로 움직임");
}

bool MarketCompare::underlyingWeekend(const CompareSpec& spec, qint64 openMs) {
    if (spec.venue != CompareVenue::BinanceUsdM || spec.underlyingType.isEmpty()) return false;
    const auto day = QDateTime::fromMSecsSinceEpoch(openMs, QTimeZone::UTC).date().dayOfWeek();
    return day == Qt::Saturday || day == Qt::Sunday;
}
} // namespace keefi
