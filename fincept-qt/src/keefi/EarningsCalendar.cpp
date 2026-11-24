#include "keefi/EarningsCalendar.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTimer>
#include <QTimeZone>
#include <QUrlQuery>
#include <algorithm>
#include <utility>

namespace keefi {
namespace {
const QTimeZone& seoul() { static const QTimeZone zone("Asia/Seoul"); return zone; }
const QTimeZone& newYork() { static const QTimeZone zone("America/New_York"); return zone; }
QDate today(qint64 now, const QTimeZone& zone) { return QDateTime::fromMSecsSinceEpoch(now, zone).date(); }
bool bounded(const QString& text, qsizetype maximum) {
    if (text.trimmed().isEmpty() || text.size() > maximum) return false;
    for (auto c : text) if (c.isNull() || c.category() == QChar::Other_Control) return false;
    return true;
}
QString cellText(QString html) {
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    html.remove(tags);
    static const QRegularExpression numeric(QStringLiteral("&#([0-9]{1,5});"));
    for (auto m = numeric.match(html); m.hasMatch(); m = numeric.match(html, m.capturedStart() + 1)) {
        const auto code = m.captured(1).toUInt();
        html.replace(m.capturedStart(), m.capturedLength(), code > 0 && code < 0xD800 ? QString(QChar(code)) : QString());
    }
    static const std::pair<const char*, const char*> entities[] = {{"&nbsp;", " "}, {"&quot;", "\""}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
    for (const auto& [entity, text] : entities) html.replace(QLatin1String(entity), QLatin1String(text));
    return html.simplified();
}
QByteArray kindBody(const QDate& from, const QDate& to, int page) {
    // The exact search form KIND's IR schedule page posts; only dates and page vary.
    return "method=searchIRScheduleSub&forward=searchirschedule_sub&currentPageSize=" + QByteArray::number(EarningsCalendar::KindPageSize) +
        "&pageIndex=" + QByteArray::number(page) + "&marketType=1&fromDate=" + from.toString(Qt::ISODate).toLatin1() +
        "&toDate=" + to.toString(Qt::ISODate).toLatin1() + "&orderMode=&orderStat=&searchCodeType=";
}
} // namespace

QString EarningsCalendar::sourceName(EarningsSource source) {
    return source == EarningsSource::Kind ? QStringLiteral("한국거래소 KIND") : QStringLiteral("Nasdaq");
}
RequestSpec EarningsCalendar::kindRequest(const QDate& from, const QDate& to, int page) {
    RequestSpec r; r.operation = Operation::Markets; r.method = "POST";
    r.url = QUrl("https://kind.krx.co.kr/corpgeneral/irschedule.do");
    r.headers.insert("Accept", "text/html"); r.headers.insert("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8");
    r.body = kindBody(from, to, page); return r;
}
RequestSpec EarningsCalendar::nasdaqRequest(const QDate& date) {
    RequestSpec r; r.operation = Operation::Markets;
    QUrl url("https://api.nasdaq.com/api/calendar/earnings"); QUrlQuery query; query.addQueryItem("date", date.toString(Qt::ISODate));
    url.setQuery(query); r.url = url;
    r.headers.insert("Accept", "application/json"); r.headers.insert("User-Agent", "Mozilla/5.0 (KEEFI read-only market map)"); return r;
}
bool EarningsCalendar::isRequest(const RequestSpec& r) {
    if (r.authenticated || r.operation != Operation::Markets) return false;
    if (r.method == "POST") {
        // Rebuild from the posted dates and page; any other field or header fails.
        const QUrlQuery form(QString::fromLatin1(r.body));
        const auto from = QDate::fromString(form.queryItemValue("fromDate"), Qt::ISODate), to = QDate::fromString(form.queryItemValue("toDate"), Qt::ISODate);
        bool ok = false; const int page = form.queryItemValue("pageIndex").toInt(&ok);
        if (!ok || !from.isValid() || !to.isValid() || from > to || from.daysTo(to) > 14 || page < 1 || page > KindMaxPages) return false;
        const auto expected = kindRequest(from, to, page);
        return r.url == expected.url && r.headers == expected.headers && r.body == expected.body;
    }
    if (r.method != "GET" || !r.body.isEmpty()) return false;
    const auto date = QDate::fromString(QUrlQuery(r.url).queryItemValue("date"), Qt::ISODate);
    if (!date.isValid()) return false;
    const auto expected = nasdaqRequest(date);
    return r.url == expected.url && r.headers == expected.headers;
}
bool EarningsCalendar::earningsTitle(const QString& title) {
    static const QRegularExpression results(QStringLiteral("실적|결산|경영성과|Results|Earnings|Performance"), QRegularExpression::CaseInsensitiveOption);
    return results.match(title).hasMatch();
}
bool EarningsCalendar::parseKind(const QByteArray& body, const QDate& from, const QDate& to, QList<EarningsEvent>* out, int* total) {
    if (!out || !total || body.isEmpty() || body.size() > 2 * 1024 * 1024 || body.contains('\0') || !from.isValid() || from > to) return false;
    QStringDecoder decoder(QStringDecoder::Utf8); const QString text = decoder(body);
    if (decoder.hasError() || !seoul().isValid()) return false;
    static const QRegularExpression count(QStringLiteral("전체\\s*<em>([0-9,]{1,7})</em>건"));
    const auto counted = count.match(text); if (!counted.hasMatch()) return false;
    bool ok = false; const int all = counted.captured(1).remove(',').toInt(&ok); if (!ok) return false;
    const auto open = text.indexOf(QStringLiteral("<tbody>")), close = text.indexOf(QStringLiteral("</tbody>"));
    if (open < 0 || close < open) return false;
    const QString rows = text.mid(open + 7, close - open - 7);
    static const QRegularExpression rowPattern(QStringLiteral("<tr[^>]*>(.*?)</tr>"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression cellPattern(QStringLiteral("<td[^>]*>(.*?)</td>"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression company(QStringLiteral("companysummary_open\\('([0-9A-Z]{5})'\\)"));
    static const QRegularExpression clock(QStringLiteral("\\A([0-9]{2}):([0-9]{2})\\z"));
    QList<EarningsEvent> parsed; int matched = 0;
    for (auto it = rowPattern.globalMatch(rows); it.hasNext();) {
        const auto row = it.next(); ++matched;
        QStringList cells; for (auto c = cellPattern.globalMatch(row.captured(1)); c.hasNext();) cells.append(c.next().captured(1));
        if (cells.size() == 1 && all == 0) continue; // "조회된 결과값이 없습니다." placeholder.
        if (cells.size() != 6) return false;
        const auto code = company.match(cells[1]);
        const auto name = cellText(cells[1]), title = cellText(cells[2]), dateText = cellText(cells[4]), timeText = cellText(cells[5]);
        const auto date = QDate::fromString(dateText, Qt::ISODate);
        if (!code.hasMatch() || !bounded(name, 128) || !bounded(title, 512) || !date.isValid() || date < from || date > to) return false;
        EarningsEvent event; event.source = EarningsSource::Kind; event.code = code.captured(1); event.name = name; event.title = title; event.date = date;
        if (timeText == QStringLiteral("--:--")) {
            event.timing = EarningsTiming::NotSupplied; event.startsAtMs = QDateTime(date, QTime(0, 0), seoul()).toMSecsSinceEpoch();
        } else {
            const auto m = clock.match(timeText); const QTime time = m.hasMatch() ? QTime(m.captured(1).toInt(), m.captured(2).toInt()) : QTime();
            if (!time.isValid()) return false;
            event.timing = EarningsTiming::Scheduled; event.startsAtMs = QDateTime(date, time, seoul()).toMSecsSinceEpoch();
        }
        parsed.append(event);
    }
    if (rows.count(QStringLiteral("<tr")) != matched || parsed.size() > KindPageSize || (all > 0 && parsed.isEmpty())) return false;
    *out = parsed; *total = all; return true;
}
bool EarningsCalendar::parseNasdaq(const QByteArray& body, const QDate& date, QList<EarningsEvent>* out) {
    if (!out || body.isEmpty() || body.size() > 4 * 1024 * 1024 || !date.isValid() || !newYork().isValid()) return false;
    QJsonParseError error; const auto document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto root = document.object();
    if (root.value("status").toObject().value("rCode").toInt() != 200) return false;
    QList<EarningsEvent> parsed;
    // No earnings that day (weekend or holiday) arrives as data:null with a 200 status.
    if (root.value("data").isNull()) { *out = parsed; return true; }
    const auto data = root.value("data").toObject();
    // asOf echoes the requested New York date; a different date is a different calendar page.
    const auto asOf = QLocale(QLocale::English).toDate(data.value("asOf").toString(), QStringLiteral("ddd, MMM d, yyyy"));
    if (asOf != date) return false;
    if (data.value("rows").isNull()) { *out = parsed; return true; }
    if (!data.value("rows").isArray()) return false;
    static const QRegularExpression symbol(QStringLiteral("\\A[A-Z][A-Z0-9.\\-/]{0,9}\\z")), money(QStringLiteral("\\A\\$[0-9]{1,3}(?:,[0-9]{3})*\\z"));
    static const QRegularExpression eps(QStringLiteral("\\A(?:\\(?\\$[0-9]{1,6}(?:\\.[0-9]{1,4})?\\)?)?\\z")), quarter(QStringLiteral("\\A(?:[A-Z][a-z]{2}/[0-9]{4})?\\z"));
    static const QRegularExpression estimates(QStringLiteral("\\A(?:[0-9]{1,3}|N/A)?\\z"));
    QSet<QString> seen;
    const QDateTime opens(date, QTime(8, 0), newYork()), closes(date, QTime(16, 30), newYork()), unknown(date, QTime(12, 0), newYork());
    for (const auto& value : data.value("rows").toArray()) {
        if (!value.isObject()) return false;
        const auto row = value.toObject();
        const auto ticker = row.value("symbol").toString(), name = row.value("name").toString(), timing = row.value("time").toString();
        const auto cap = row.value("marketCap").toString(), forecast = row.value("epsForecast").toString(), fiscal = row.value("fiscalQuarterEnding").toString();
        const auto analysts = row.value("noOfEsts").toString();
        if (!symbol.match(ticker).hasMatch() || seen.contains(ticker) || !bounded(name, 256) || !eps.match(forecast).hasMatch() ||
            !quarter.match(fiscal).hasMatch() || !estimates.match(analysts).hasMatch() || (!cap.isEmpty() && cap != "N/A" && !money.match(cap).hasMatch())) return false;
        EarningsEvent event; event.source = EarningsSource::Nasdaq; event.code = ticker; event.name = name.simplified(); event.date = date;
        if (timing == "time-pre-market") { event.timing = EarningsTiming::PreMarket; event.startsAtMs = opens.toMSecsSinceEpoch(); }
        else if (timing == "time-after-hours") { event.timing = EarningsTiming::AfterHours; event.startsAtMs = closes.toMSecsSinceEpoch(); }
        else if (timing == "time-not-supplied") { event.timing = EarningsTiming::NotSupplied; event.startsAtMs = unknown.toMSecsSinceEpoch(); }
        else return false;
        event.marketCapUsd = money.match(cap).hasMatch() ? QString(cap).remove('$').remove(',').toDouble() : 0;
        event.fiscalQuarter = fiscal; event.epsForecast = forecast; event.estimates = analysts == "N/A" ? QString{} : analysts;
        seen.insert(ticker); parsed.append(event);
    }
    *out = parsed; return true;
}

struct EarningsCalendar::Private {
    struct Job { EarningsSource source; QDate from, to; int page = 1; };
    EarningsCalendar* q; QPointer<TradingTransport> transport; Clock clock; QTimer timer;
    bool active = false, pending = false; quint64 token = 0;
    QList<Job> queue; Job current;
    QMap<EarningsSource, EarningsFeed> feeds;
    QMap<EarningsSource, QList<EarningsEvent>> staged;
    QMap<EarningsSource, bool> failed;
    RequestSpec request(const Job& job) const {
        return job.source == EarningsSource::Kind ? EarningsCalendar::kindRequest(job.from, job.to, job.page) : EarningsCalendar::nasdaqRequest(job.from);
    }
    void next() {
        if (!active || pending || queue.isEmpty() || !transport) return;
        current = queue.takeFirst(); pending = true; transport->send(++token, request(current));
    }
    bool remaining(EarningsSource source) const {
        return std::any_of(queue.cbegin(), queue.cend(), [&](const Job& job) { return job.source == source; });
    }
    void fail(EarningsSource source, const QString& reason) {
        // One failed page or day makes the whole source unavailable: no partial week.
        failed.insert(source, true); staged.remove(source);
        queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const Job& job) { return job.source == source; }), queue.end());
        auto& f = feeds[source]; f = {}; f.source = source; f.reason = reason;
    }
    void complete(EarningsSource source) {
        if (failed.value(source) || remaining(source)) return;
        auto events = staged.take(source);
        if (source == EarningsSource::Kind) {
            // KIND often lists one session twice (Korean and English titles); keep the Korean one.
            const auto hangul = [](const QString& text) { return std::any_of(text.cbegin(), text.cend(), [](QChar c) { return c.unicode() >= 0xAC00 && c.unicode() <= 0xD7A3; }); };
            QMap<QString, EarningsEvent> unique;
            for (const auto& event : events) {
                const auto key = event.code + '|' + QString::number(event.startsAtMs);
                if (!unique.contains(key) || (!hangul(unique[key].title) && hangul(event.title))) unique[key] = event;
            }
            events = unique.values();
        }
        std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.startsAtMs < b.startsAtMs; });
        auto& f = feeds[source]; f = {}; f.source = source; f.available = true; f.receivedAtMs = clock(); f.events = events;
    }
    void clearPending() {
        ++token; pending = false; queue.clear(); staged.clear();
        if (transport) transport->abort();
        for (auto& f : feeds) if (f.loading) { f.loading = false; f.reason = QStringLiteral("실적 일정 조회 중단"); }
    }
};
EarningsCalendar::EarningsCalendar(TradingTransport* transport, QObject* parent, Clock clock)
    : QObject(parent), d_(std::make_unique<Private>()) {
    d_->q = this; d_->transport = transport; d_->clock = clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); };
    for (auto source : {EarningsSource::Kind, EarningsSource::Nasdaq}) {
        EarningsFeed f; f.source = source; f.reason = QStringLiteral("실적 일정 조회 전"); d_->feeds.insert(source, f);
    }
    d_->timer.setInterval(60 * 60000); connect(&d_->timer, &QTimer::timeout, this, &EarningsCalendar::refresh);
    if (transport) connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& response) {
        if (!d_->active || !d_->pending || token != d_->token) return;
        d_->pending = false; const auto job = d_->current; const auto source = job.source;
        if (d_->failed.value(source)) { QTimer::singleShot(0, this, [this] { d_->next(); }); return; }
        if (response.status != 200 || response.networkError || response.timedOut) {
            d_->fail(source, response.status > 0 ? QStringLiteral("실적 일정 조회 실패 · HTTP %1").arg(response.status) : QStringLiteral("실적 일정 연결 끊김"));
        } else if (source == EarningsSource::Kind) {
            QList<EarningsEvent> page; int total = 0;
            if (!parseKind(response.body, job.from, job.to, &page, &total)) d_->fail(source, QStringLiteral("실적 일정 형식을 확인할 수 없습니다."));
            else {
                d_->staged[source].append(page);
                const bool more = job.page * KindPageSize < total;
                if (more && job.page >= KindMaxPages) d_->fail(source, QStringLiteral("실적 일정이 너무 많아 전체를 확인하지 못했습니다."));
                else if (more) d_->queue.prepend({source, job.from, job.to, job.page + 1});
            }
        } else {
            QList<EarningsEvent> day;
            if (!parseNasdaq(response.body, job.from, &day)) d_->fail(source, QStringLiteral("실적 일정 형식을 확인할 수 없습니다."));
            else d_->staged[source].append(day);
        }
        d_->complete(source); emit changed(); QTimer::singleShot(0, this, [this] { d_->next(); });
    });
}
EarningsCalendar::~EarningsCalendar() { d_->active = false; d_->clearPending(); }
void EarningsCalendar::setActive(bool active) {
    if (d_->active == active) return; d_->active = active;
    if (!active) { d_->timer.stop(); d_->clearPending(); emit changed(); return; }
    d_->timer.start(); refresh();
}
void EarningsCalendar::refresh() {
    if (!d_->active || d_->pending) return;
    const auto now = d_->clock(); const auto korea = today(now, seoul()), us = today(now, newYork());
    d_->queue = {{EarningsSource::Kind, korea, korea.addDays(Days), 1}};
    for (int day = 0; day <= Days; ++day) {
        const auto date = us.addDays(day);
        if (date.dayOfWeek() <= 5) d_->queue.append({EarningsSource::Nasdaq, date, date, 1}); // US markets report on weekdays.
    }
    d_->failed.clear(); d_->staged.clear();
    for (auto source : {EarningsSource::Kind, EarningsSource::Nasdaq}) {
        auto& f = d_->feeds[source]; f = {}; f.source = source; f.loading = bool(d_->transport);
        f.reason = d_->transport ? QStringLiteral("실적 일정 불러오는 중") : QStringLiteral("공개 일정 조회 연결 없음");
    }
    emit changed(); d_->next();
}
EarningsFeed EarningsCalendar::feed(EarningsSource source, qint64 now) const {
    auto result = d_->feeds.value(source); if (!now) now = d_->clock();
    if (result.available && (result.receivedAtMs > now || now - result.receivedAtMs > FreshnessMs)) {
        result.available = false; result.events.clear(); result.reason = QStringLiteral("일정 수신 후 6시간 경과 또는 시각 불일치 · 다시 조회 필요");
    }
    if (!result.available) { result.events.clear(); return result; }
    const auto first = today(now, source == EarningsSource::Kind ? seoul() : newYork());
    result.events.removeIf([&](const auto& event) { return event.date < first || event.date > first.addDays(Days); });
    return result;
}
} // namespace keefi
