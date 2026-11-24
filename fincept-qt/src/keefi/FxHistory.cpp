#include "keefi/FxHistory.h"

#include "keefi/TodayBtcPremium.h"
#include "keefi/XmlInputGuard.h"

#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QTimeZone>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>

namespace keefi {
namespace {
constexpr qint64 Day = 86400000;

bool rateText(const QString& text, double* value) {
    static const QRegularExpression exact("\\A[0-9]{1,10}(?:\\.[0-9]{1,10})?\\z");
    if (!exact.match(text).hasMatch()) return false;
    bool ok = false;
    const auto parsed = text.toDouble(&ok);
    if (!ok || !(parsed > 0) || !std::isfinite(parsed)) return false;
    *value = parsed;
    return true;
}

const FxRate* latestKnown(const QList<FxRate>& rates, qint64 atMs) {
    const auto it = std::upper_bound(rates.cbegin(), rates.cend(), atMs,
                                     [](qint64 at, const FxRate& rate) { return at < rate.knownAtMs; });
    if (it == rates.cbegin()) return nullptr;
    return &*(it - 1);
}
} // namespace

RequestSpec FxHistory::ecbHistoryRequest() {
    RequestSpec request;
    request.operation = Operation::Markets;
    request.url = QUrl(QStringLiteral("https://www.ecb.europa.eu/stats/eurofxref/eurofxref-hist.xml"));
    request.headers.insert("Accept", "application/xml");
    return request;
}

bool FxHistory::isRequest(const RequestSpec& request) {
    const auto expected = ecbHistoryRequest();
    return request.method == "GET" && !request.authenticated && request.body.isEmpty() && request.operation == expected.operation &&
           request.url == expected.url && request.headers == expected.headers;
}

qint64 FxHistory::ecbKnownAtMs(const QDate& date) {
    if (!date.isValid()) return 0;
    return QDateTime(date, QTime(17, 0), QTimeZone("Europe/Berlin")).toMSecsSinceEpoch();
}

QList<FxRate> FxHistory::parseEcb(const QByteArray& xml, qint64 receivedAtMs, QString* reason) {
    const auto fail = [&](const QString& why) { if (reason) *reason = why; return QList<FxRate>{}; };
    if (receivedAtMs <= 0 || xml.isEmpty() || xml.size() > MaximumEcbBytes || !xml.isValidUtf8())
        return fail(QStringLiteral("ECB 응답 형식 확인 필요"));
    if (!xmlAttributeBudgetAllows(xml)) return fail(QStringLiteral("ECB XML 형식 확인 필요"));
    QXmlStreamReader reader(xml);
    QMap<QDate, QHash<QString, QString>> days;
    QDate current;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference)
            return fail(QStringLiteral("ECB XML 형식 확인 필요"));
        if (token != QXmlStreamReader::StartElement || reader.name() != QLatin1String("Cube")) continue;
        const auto attributes = reader.attributes();
        if (attributes.hasAttribute("time")) {
            current = QDate::fromString(attributes.value("time").toString(), Qt::ISODate);
            if (!current.isValid()) return fail(QStringLiteral("ECB 날짜 형식 확인 필요"));
            continue;
        }
        if (attributes.hasAttribute("currency") && current.isValid()) {
            const auto currency = attributes.value("currency").toString();
            if (currency == "USD" || currency == "KRW") days[current].insert(currency, attributes.value("rate").toString());
        }
    }
    if (reader.hasError()) return fail(QStringLiteral("ECB XML 형식 확인 필요"));
    QList<FxRate> out;
    for (auto it = days.cbegin(); it != days.cend(); ++it) {
        double usd = 0, krw = 0;
        if (!rateText(it.value().value("USD"), &usd) || !rateText(it.value().value("KRW"), &krw)) continue;
        FxRate rate;
        rate.date = it.key();
        rate.value = krw / usd;
        rate.krwPerUsd = QString::number(rate.value, 'f', 4);
        rate.postedAtMs = QDateTime(it.key(), QTime(16, 0), QTimeZone("Europe/Berlin")).toMSecsSinceEpoch();
        rate.knownAtMs = ecbKnownAtMs(it.key());
        rate.receivedAtMs = receivedAtMs;
        rate.source = QStringLiteral("ecb");
        out.append(rate);
    }
    if (out.isEmpty()) return fail(QStringLiteral("ECB 원/달러 값이 없어요"));
    if (reason) reason->clear();
    return out;
}

FxRate FxHistory::fromHana(const UsdKrwPosting& posting) {
    FxRate rate;
    double value = 0;
    if (!rateText(posting.krwPerUsd, &value) || posting.postedAtMs <= 0 || posting.receivedAtMs <= 0) return rate;
    rate.date = posting.postedDate;
    rate.krwPerUsd = posting.krwPerUsd;
    rate.value = value;
    rate.postedAtMs = posting.postedAtMs;
    rate.receivedAtMs = posting.receivedAtMs;
    rate.knownAtMs = posting.receivedAtMs; // First receipt; never the posting time.
    rate.source = QStringLiteral("hana");
    return rate;
}

FxChoice FxHistory::choose(qint64 barCloseMs, qint64 intervalMs, const QList<FxRate>& ecb, const QList<FxRate>& hana) {
    FxChoice choice;
    choice.label = QStringLiteral("환산 불가");
    if (barCloseMs <= 0 || intervalMs <= 0) return choice;
    const auto barEnd = barCloseMs + 1;
    const auto barDate = QDateTime::fromMSecsSinceEpoch(barEnd - intervalMs, QTimeZone::UTC).date();
    const auto useEcb = [&](bool approximate) {
        const auto* rate = latestKnown(ecb, barEnd);
        if (!rate) return false;
        choice.available = true;
        choice.approximate = approximate;
        choice.krwPerUsd = rate->value;
        choice.source = rate->source;
        choice.postedAtMs = rate->postedAtMs;
        choice.knownAtMs = rate->knownAtMs;
        choice.date = rate->date;
        choice.previous = rate->date < barDate;
        choice.label = approximate ? QStringLiteral("ECB 일별(근사)") : QStringLiteral("ECB 일별");
        return true;
    };
    if (intervalMs >= Day) { useEcb(false); return choice; }
    if (const auto* rate = latestKnown(hana, barEnd); rate && barEnd - rate->postedAtMs <= HanaMaximumAgeMs) {
        choice.available = true;
        choice.krwPerUsd = rate->value;
        choice.source = rate->source;
        choice.postedAtMs = rate->postedAtMs;
        choice.knownAtMs = rate->knownAtMs;
        choice.date = rate->date;
        choice.previous = barEnd - rate->postedAtMs > HanaPreviousAfterMs;
        choice.label = QStringLiteral("하나은행 고시");
        return choice;
    }
    useEcb(true);
    return choice;
}
} // namespace keefi
