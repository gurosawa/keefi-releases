#include "keefi/OfficialCalendar.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTimer>
#include <QTimeZone>
#include <QUrlQuery>
#include <algorithm>
namespace keefi {
namespace {
struct Property { QString name, value; QMap<QString, QString> parameters; };
bool parseProperty(const QString& line, Property& p) {
    const auto colon = line.indexOf(':'); if (colon <= 0) return false;
    const auto parts = line.left(colon).split(';'); p.name = parts[0].toUpper(); p.value = line.mid(colon + 1);
    for (int i = 1; i < parts.size(); ++i) {
        const auto equal = parts[i].indexOf('='); if (equal <= 0) return false;
        const auto name = parts[i].left(equal).toUpper(); auto value = parts[i].mid(equal + 1);
        if (value.startsWith('"') && value.endsWith('"')) value = value.mid(1, value.size() - 2);
        if (p.parameters.contains(name) || value.isEmpty()) return false;
        p.parameters.insert(name, value);
    }
    return true;
}
qint64 dateTime(const Property& p, QString* zoneName = nullptr) {
    if ((!p.parameters.value("VALUE").isEmpty() && p.parameters.value("VALUE") != "DATE-TIME") ||
        std::any_of(p.parameters.keyBegin(), p.parameters.keyEnd(), [](const auto& key) { return key != "VALUE" && key != "TZID"; })) return 0;
    const auto match = QRegularExpression("\\A([0-9]{8})T([0-9]{6})(Z?)\\z").match(p.value);
    if (!match.hasMatch()) return 0;
    const bool utc = !match.captured(3).isEmpty();
    const auto timezone = p.parameters.value("TZID");
    if ((utc && !timezone.isEmpty()) || (!utc && timezone.isEmpty())) return 0;
    const QTimeZone zone(utc ? QByteArray("UTC") : timezone.toUtf8());
    if (!zone.isValid()) return 0;
    const QDate date = QDate::fromString(match.captured(1), "yyyyMMdd");
    const QTime time = QTime::fromString(match.captured(2), "HHmmss");
    if (!date.isValid() || !time.isValid()) return 0;
    const QDateTime result(date, time, zone, QDateTime::TransitionResolution::Reject);
    if (!result.isValid()) return 0;
    if (zoneName) *zoneName = utc ? QStringLiteral("UTC") : timezone;
    return result.toMSecsSinceEpoch();
}
QString unescape(const QString& input, bool* ok) {
    QString result; *ok = true;
    for (int i = 0; i < input.size(); ++i) {
        auto c = input[i];
        if (c == '\\') {
            if (++i == input.size()) { *ok = false; return {}; }
            c = input[i];
            if (c == 'n' || c == 'N') c = ' ';
            else if (c != '\\' && c != ',' && c != ';') { *ok = false; return {}; }
        }
        result.append(c);
    }
    return result.simplified();
}
bool sameEvent(const OfficialCalendarEvent& a, const OfficialCalendarEvent& b) {
    return a.startsAtMs == b.startsAtMs && a.title == b.title && a.cancelled == b.cancelled;
}
bool decodeEvent(const QMap<QString, Property>& properties, qint64 received, OfficialCalendarEvent& event) {
    for (const auto* unsupported : {"RRULE", "RDATE", "EXDATE", "RECURRENCE-ID"}) if (properties.contains(unsupported)) return false;
    event.uid = properties.value("UID").value;
    if (event.uid.trimmed().isEmpty() || event.uid.size() > 512) return false;
    if (properties.contains("SEQUENCE")) {
        bool ok = false; event.sequence = properties.value("SEQUENCE").value.toInt(&ok);
        if (!ok || event.sequence < 0) return false;
    }
    const auto status = properties.value("STATUS").value;
    if (!status.isEmpty() && status != "CONFIRMED" && status != "TENTATIVE" && status != "CANCELLED") return false;
    event.cancelled = status == "CANCELLED";
    event.updatedAtMs = dateTime(properties.value("DTSTAMP"));
    if (event.updatedAtMs <= 0 || event.updatedAtMs > received) return false;
    if (properties.contains("LAST-MODIFIED")) {
        const auto modified = dateTime(properties.value("LAST-MODIFIED"));
        if (modified <= 0 || modified > received) return false;
        event.updatedAtMs = qMax(event.updatedAtMs, modified);
    }
    if (event.cancelled && !properties.contains("DTSTART")) return true;
    bool ok = false; event.title = unescape(properties.value("SUMMARY").value, &ok);
    event.startsAtMs = dateTime(properties.value("DTSTART"), &event.sourceTimeZone);
    return ok && !event.title.isEmpty() && event.title.size() <= 2048 && event.startsAtMs > 0;
}
}
QString OfficialCalendar::sourceName(OfficialCalendarSource source) {
    switch (source) {
    case OfficialCalendarSource::Bls: return QStringLiteral("미 노동통계국 BLS");
    case OfficialCalendarSource::Bea: return QStringLiteral("미 경제분석국 BEA");
    case OfficialCalendarSource::Fomc: return QStringLiteral("미 연준 FOMC");
    case OfficialCalendarSource::Bok: return QStringLiteral("한국은행 금통위");
    }
    return {};
}
RequestSpec OfficialCalendar::request(OfficialCalendarSource source, int year) {
    RequestSpec r; r.operation = Operation::Markets;
    switch (source) {
    case OfficialCalendarSource::Bok: {
        if (year <= 0) year = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Asia/Seoul")).date().year();
        QUrl url("https://www.bok.or.kr/portal/singl/crncyPolicyDrcMtg/listYear.do"); QUrlQuery query;
        query.addQueryItem("mtgSe", "A"); query.addQueryItem("menuNo", "200755"); query.addQueryItem("pYear", QString::number(year));
        url.setQuery(query); r.url = url; r.headers.insert("Accept", "text/html"); return r;
    }
    case OfficialCalendarSource::Bls: r.url = QUrl("https://www.bls.gov/schedule/news_release/bls.ics"); break;
    case OfficialCalendarSource::Bea: r.url = QUrl("https://www.bea.gov/news/schedule/ics/online-calendar-subscription.ics"); break;
    case OfficialCalendarSource::Fomc:
        r.url = QUrl("https://www.federalreserve.gov/monetarypolicy/fomccalendars.htm"); r.headers.insert("Accept", "text/html"); return r;
    default: return r;
    }
    r.headers.insert("Accept", "text/calendar, text/plain"); return r;
}
bool OfficialCalendar::isRequest(const RequestSpec& r) {
    if (r.method != "GET" || r.authenticated || !r.body.isEmpty() || r.operation != Operation::Markets) return false;
    for (auto source : {OfficialCalendarSource::Bls, OfficialCalendarSource::Bea, OfficialCalendarSource::Fomc}) {
        const auto expected = request(source);
        if (r.url == expected.url && r.headers == expected.headers) return true;
    }
    bool ok = false; const int year = QUrlQuery(r.url).queryItemValue("pYear").toInt(&ok);
    if (ok && year >= 2000 && year <= 2099) {
        const auto expected = request(OfficialCalendarSource::Bok, year);
        if (r.url == expected.url && r.headers == expected.headers) return true;
    }
    return false;
}
bool OfficialCalendar::parseFomc(const QByteArray& body, qint64 received, OfficialCalendarFeed* out) {
    if (!out) return false;
    *out = {}; out->source = OfficialCalendarSource::Fomc; out->reason = QStringLiteral("공식 일정 형식·시각을 확인할 수 없습니다.");
    OfficialCalendarFeed parsed = *out; // Copied out only when every row is accepted.
    if (received <= 0 || body.isEmpty() || body.size() > 2 * 1024 * 1024 || body.contains('\0')) return false;
    QStringDecoder decoder(QStringDecoder::Utf8); const QString text = decoder(body);
    if (decoder.hasError()) return false;
    static const QStringList months{"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"};
    const auto month = [](QString name) { name = name.trimmed().toLower(); for (int i = 0; i < months.size(); ++i) if (name == months[i] || (name.size() == 3 && months[i].startsWith(name))) return i + 1; return 0; };
    const QTimeZone eastern("America/New_York"); if (!eastern.isValid()) return false;
    static const QRegularExpression updated(QStringLiteral("Last Update:\\s*([A-Za-z]+)\\s+([0-9]{1,2}),\\s*([0-9]{4})"));
    if (const auto m = updated.match(text); m.hasMatch()) {
        const QDate date(m.captured(3).toInt(), month(m.captured(1)), m.captured(2).toInt());
        if (date.isValid()) parsed.sourceUpdatedAtMs = QDateTime(date, QTime(0, 0), eastern).toMSecsSinceEpoch();
        if (parsed.sourceUpdatedAtMs > received) parsed.sourceUpdatedAtMs = 0;
    }
    static const QRegularExpression panel(QStringLiteral("<h4><a[^>]*>([0-9]{4}) FOMC Meetings</a></h4>"));
    static const QRegularExpression row(QStringLiteral("class=\"[^\"]*fomc-meeting__month[^\"]*\"><strong>([A-Za-z/]+)</strong></div>\\s*<div class=\"[^\"]*fomc-meeting__date[^\"]*\">([^<]*)</div>"));
    static const QRegularExpression regular(QStringLiteral("\\A([0-9]{1,2})-([0-9]{1,2})\\*?\\z"));
    static const QRegularExpression excluded(QStringLiteral("\\A[0-9]{1,2}(?:-[0-9]{1,2})? \\((?:unscheduled|notation vote)\\)\\z"));
    QList<QRegularExpressionMatch> panels; for (auto it = panel.globalMatch(text); it.hasNext();) panels.append(it.next());
    if (panels.isEmpty()) return false;
    // All or nothing: an unreadable row, or no regular meeting for the current
    // year, means the page changed, so the whole source is unavailable.
    const int currentYear = QDateTime::fromMSecsSinceEpoch(received, eastern).date().year();
    bool currentYearSeen = false;
    for (int p = 0; p < panels.size(); ++p) {
        const int year = panels[p].captured(1).toInt();
        const auto from = panels[p].capturedEnd(), until = p + 1 < panels.size() ? panels[p + 1].capturedStart() : text.size();
        const QString section = text.mid(from, until - from);
        int rows = 0, regularRows = 0;
        for (auto it = row.globalMatch(section); it.hasNext();) {
            const auto m = it.next(); ++rows; const auto dateText = m.captured(2).trimmed(); const auto days = regular.match(dateText);
            if (!days.hasMatch()) { if (excluded.match(dateText).hasMatch()) continue; return false; } // Unscheduled meetings and notation votes have no regular statement time.
            ++regularRows;
            const auto names = m.captured(1).split('/');
            if (names.size() > 2) return false;
            const int first = month(names.first()), second = month(names.last());
            const int start = days.captured(1).toInt(), end = days.captured(2).toInt();
            if (!first || !second || (names.size() == 1 && end <= start) || (names.size() == 2 && second != first % 12 + 1)) return false;
            const QDate date(second < first ? year + 1 : year, second, end);
            if (!date.isValid() || !QDate(year, first, start).isValid()) return false;
            const QDateTime statement(date, QTime(14, 0), eastern, QDateTime::TransitionResolution::Reject);
            if (!statement.isValid()) return false;
            OfficialCalendarEvent event; event.uid = QStringLiteral("fomc-") + date.toString(Qt::ISODate); event.title = QStringLiteral("FOMC Statement");
            event.sourceTimeZone = QStringLiteral("America/New_York"); event.startsAtMs = statement.toMSecsSinceEpoch(); event.updatedAtMs = parsed.sourceUpdatedAtMs;
            if (std::none_of(parsed.events.cbegin(), parsed.events.cend(), [&](const auto& e) { return e.uid == event.uid; })) parsed.events.append(event);
        }
        if (rows != section.count(QStringLiteral("fomc-meeting__month"))) return false;
        if (year == currentYear && regularRows > 0) currentYearSeen = true;
    }
    if (parsed.events.isEmpty() || !currentYearSeen) return false;
    std::sort(parsed.events.begin(), parsed.events.end(), [](const auto& a, const auto& b) { return a.startsAtMs < b.startsAtMs; });
    parsed.available = true; parsed.receivedAtMs = received; parsed.reason.clear(); *out = parsed; return true;
}
bool OfficialCalendar::parseBok(const QByteArray& body, qint64 received, OfficialCalendarFeed* out) {
    if (!out) return false;
    *out = {}; out->source = OfficialCalendarSource::Bok; out->reason = QStringLiteral("공식 일정 형식·날짜를 확인할 수 없습니다.");
    if (received <= 0 || body.isEmpty() || body.size() > 2 * 1024 * 1024 || body.contains('\0')) return false;
    QStringDecoder decoder(QStringDecoder::Utf8); const QString text = decoder(body);
    if (decoder.hasError()) return false;
    const QTimeZone kst("Asia/Seoul"); if (!kst.isValid()) return false;
    // The page names its list year once; it must be the KST year the list was requested for.
    static const QRegularExpression heading(QStringLiteral("<h3>([0-9]{4})년</h3>"));
    QList<QRegularExpressionMatch> years; for (auto it = heading.globalMatch(text); it.hasNext();) years.append(it.next());
    if (years.size() != 1) return false;
    const int year = years.first().captured(1).toInt();
    if (year != QDateTime::fromMSecsSinceEpoch(received, kst).date().year()) return false;
    const auto table = text.indexOf(QStringLiteral("<table id=\"tableId\">"));
    const auto open = table < 0 ? -1 : text.indexOf(QStringLiteral("<tbody>"), table);
    const auto close = open < 0 ? -1 : text.indexOf(QStringLiteral("</tbody>"), open);
    if (close < 0) return false;
    const QString rows = text.mid(open + 7, close - open - 7);
    static const QRegularExpression row(QStringLiteral("<tr[^>]*>\\s*<th scope=\"row\">\\s*([0-9]{2})월 ([0-9]{2})일\\(([월화수목금토일])\\)\\s*</th>"));
    static const QString weekdays = QStringLiteral("월화수목금토일");
    OfficialCalendarFeed parsed = *out; QDate previous; int matched = 0;
    for (auto it = row.globalMatch(rows); it.hasNext();) {
        const auto m = it.next(); ++matched;
        const QDate date(year, m.captured(1).toInt(), m.captured(2).toInt());
        // All or nothing: an impossible date, wrong weekday or out-of-order row means the page changed.
        if (!date.isValid() || weekdays.at(date.dayOfWeek() - 1) != m.captured(3).at(0) || (previous.isValid() && date <= previous)) return false;
        previous = date;
        OfficialCalendarEvent event; event.uid = QStringLiteral("bok-") + date.toString(Qt::ISODate);
        event.title = QStringLiteral("한국은행 통화정책방향 결정회의"); event.sourceTimeZone = QStringLiteral("Asia/Seoul");
        event.startsAtMs = QDateTime(date, QTime(0, 0), kst).toMSecsSinceEpoch(); event.dateOnly = true; parsed.events.append(event);
    }
    if (matched == 0 || matched > 12 || matched != rows.count(QStringLiteral("<tr"))) return false;
    parsed.available = true; parsed.receivedAtMs = received; parsed.reason.clear(); *out = parsed; return true;
}
bool OfficialCalendar::parse(OfficialCalendarSource source, const QByteArray& body, qint64 received, OfficialCalendarFeed* out) {
    if (source == OfficialCalendarSource::Fomc) return parseFomc(body, received, out);
    if (source == OfficialCalendarSource::Bok) return parseBok(body, received, out);
    if (!out) return false;
    *out = {}; out->source = source; out->reason = QStringLiteral("공식 일정 형식·시각을 확인할 수 없습니다.");
    if (received <= 0 || body.isEmpty() || body.size() > 1024 * 1024 || body.contains('\0')) return false;
    QList<QByteArray> lines;
    for (auto line : body.split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.startsWith(' ') || line.startsWith('\t')) {
            if (lines.isEmpty()) return false;
            lines.last().append(line.mid(1)); if (lines.last().size() > 16384) return false;
        } else if (!line.isEmpty()) { if (line.size() > 16384) return false; lines.append(line); }
    }
    if (lines.isEmpty() || lines.first() != "BEGIN:VCALENDAR" || lines.last() != "END:VCALENDAR") return false;
    QMap<QString, OfficialCalendarEvent> versions;
    QMap<QString, Property> properties;
    QStringList stack; bool calendarSeen = false, versionSeen = false; int count = 0;
    for (const auto& bytes : lines) {
        QStringDecoder decoder(QStringDecoder::Utf8); const QString line = decoder(bytes);
        if (decoder.hasError()) return false;
        Property p; if (!parseProperty(line, p)) return false;
        if (p.name == "BEGIN") {
            if (p.value == "VCALENDAR") { if (calendarSeen || !stack.isEmpty()) return false; calendarSeen = true; }
            else if (p.value == "VEVENT") { if (stack != QStringList{"VCALENDAR"}) return false; properties.clear(); }
            else if (p.value == "VTIMEZONE") { if (stack != QStringList{"VCALENDAR"}) return false; }
            else if (p.value == "STANDARD" || p.value == "DAYLIGHT") { if (stack != QStringList{"VCALENDAR", "VTIMEZONE"}) return false; }
            else return false;
            stack.append(p.value); continue;
        }
        if (p.name == "END") {
            if (stack.isEmpty() || stack.last() != p.value) return false;
            if (p.value == "VEVENT") {
                OfficialCalendarEvent event;
                if (++count > 2048 || !decodeEvent(properties, received, event)) return false;
                const auto previous = versions.constFind(event.uid);
                if (previous == versions.cend()) versions.insert(event.uid, event);
                else if (event.sequence > previous->sequence ||
                    (event.sequence == previous->sequence && event.updatedAtMs > previous->updatedAtMs)) versions[event.uid] = event;
                else if (event.sequence == previous->sequence && event.updatedAtMs == previous->updatedAtMs && !sameEvent(event, *previous)) return false;
            }
            stack.removeLast(); continue;
        }
        if (stack == QStringList{"VCALENDAR"}) {
            if (p.name == "VERSION") { if (versionSeen || p.value != "2.0") return false; versionSeen = true; }
            if (p.name == "METHOD" && p.value != "PUBLISH") return false;
        } else if (stack == QStringList{"VCALENDAR", "VEVENT"}) {
            if (properties.contains(p.name)) return false;
            properties.insert(p.name, p);
        } else if (stack.isEmpty()) return false;
    }
    if (!stack.isEmpty() || !versionSeen) return false;
    for (const auto& event : versions) {
        out->sourceUpdatedAtMs = qMax(out->sourceUpdatedAtMs, event.updatedAtMs);
        if (!event.cancelled) out->events.append(event);
    }
    std::sort(out->events.begin(), out->events.end(), [](const auto& a, const auto& b) {
        return a.startsAtMs != b.startsAtMs ? a.startsAtMs < b.startsAtMs : a.uid < b.uid;
    });
    out->available = true; out->receivedAtMs = received; out->reason.clear(); return true;
}

struct OfficialCalendar::Private {
    OfficialCalendar* q; QPointer<TradingTransport> transport; Clock clock; QTimer timer;
    bool active = false, pending = false; quint64 token = 0;
    QList<OfficialCalendarSource> queue;
    QMap<OfficialCalendarSource, OfficialCalendarFeed> feeds;
    OfficialCalendarSource current = OfficialCalendarSource::Bls;
    void next() {
        if (!active || pending || queue.isEmpty() || !transport) return;
        current = queue.takeFirst(); pending = true;
        const auto year = QDateTime::fromMSecsSinceEpoch(clock(), QTimeZone("Asia/Seoul")).date().year();
        transport->send(++token, OfficialCalendar::request(current, year));
    }
    void clearPending() {
        ++token; pending = false; queue.clear();
        if (transport) transport->abort();
        for (auto& f : feeds) if (f.loading) { f.loading = false; f.reason = QStringLiteral("일정 조회 중단"); }
    }
};
OfficialCalendar::OfficialCalendar(TradingTransport* transport, QObject* parent, Clock clock)
    : QObject(parent), d_(std::make_unique<Private>()) {
    d_->q = this; d_->transport = transport; d_->clock = clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); };
    for (auto source : {OfficialCalendarSource::Bls, OfficialCalendarSource::Bea, OfficialCalendarSource::Fomc, OfficialCalendarSource::Bok}) {
        OfficialCalendarFeed f; f.source = source; f.reason = QStringLiteral("공식 일정 조회 전"); d_->feeds.insert(source, f);
    }
    d_->timer.setInterval(30 * 60000); connect(&d_->timer, &QTimer::timeout, this, &OfficialCalendar::refresh);
    if (transport) connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& response) {
        if (!d_->active || !d_->pending || token != d_->token) return;
        auto& f = d_->feeds[d_->current]; const auto source = d_->current;
        f = {}; f.source = source; d_->pending = false;
        if (response.status == 200 && !response.networkError && !response.timedOut)
            parse(source, response.body, d_->clock(), &f);
        else f.reason = response.status > 0 ? QStringLiteral("공식 일정 조회 실패 · HTTP %1").arg(response.status) : QStringLiteral("공식 일정 연결 끊김");
        emit changed(); QTimer::singleShot(0, this, [this] { d_->next(); });
    });
}
OfficialCalendar::~OfficialCalendar() { d_->active = false; d_->clearPending(); }
void OfficialCalendar::setActive(bool active) {
    if (d_->active == active) return; d_->active = active;
    if (!active) { d_->timer.stop(); d_->clearPending(); emit changed(); return; }
    d_->timer.start(); refresh();
}
void OfficialCalendar::refresh() {
    if (!d_->active || d_->pending) return;
    d_->queue = {OfficialCalendarSource::Bls, OfficialCalendarSource::Bea, OfficialCalendarSource::Fomc, OfficialCalendarSource::Bok};
    for (auto source : d_->queue) {
        auto& f = d_->feeds[source]; f = {}; f.source = source; f.loading = bool(d_->transport);
        f.reason = d_->transport ? QStringLiteral("공식 일정 불러오는 중") : QStringLiteral("공개 일정 조회 연결 없음");
    }
    emit changed(); d_->next();
}
OfficialCalendarFeed OfficialCalendar::feed(OfficialCalendarSource source, qint64 now) const {
    auto result = d_->feeds.value(source); if (!now) now = d_->clock();
    if (result.available && (result.receivedAtMs > now || now - result.receivedAtMs > FreshnessMs)) {
        result.available = false; result.events.clear(); result.reason = QStringLiteral("일정 수신 후 6시간 경과 또는 시각 불일치 · 다시 조회 필요");
    }
    if (!result.available) { result.events.clear(); return result; }
    const QTimeZone kst("Asia/Seoul"); const auto date = QDateTime::fromMSecsSinceEpoch(now, kst).date();
    const auto from = QDateTime(date, QTime(0, 0), kst).toMSecsSinceEpoch();
    const auto until = QDateTime(date.addDays(31), QTime(0, 0), kst).toMSecsSinceEpoch();
    result.events.removeIf([&](const auto& event) { return event.startsAtMs < from || event.startsAtMs >= until; });
    return result;
}
OfficialCalendarFeed OfficialCalendar::weekFeed(OfficialCalendarSource source, qint64 now) const {
    auto result = d_->feeds.value(source); if (!now) now = d_->clock();
    if (result.available && (result.receivedAtMs > now || now - result.receivedAtMs > FreshnessMs)) {
        result.available = false; result.events.clear(); result.reason = QStringLiteral("일정 수신 후 6시간 경과 또는 시각 불일치 · 다시 조회 필요");
    }
    if (!result.available) { result.events.clear(); return result; }
    const QTimeZone kst("Asia/Seoul"); const auto date = QDateTime::fromMSecsSinceEpoch(now, kst).date();
    const auto monday = date.addDays(1 - date.dayOfWeek());
    const auto from = QDateTime(monday, QTime(0, 0), kst).toMSecsSinceEpoch();
    const auto until = QDateTime(date.addDays(31), QTime(0, 0), kst).toMSecsSinceEpoch();
    result.events.removeIf([&](const auto& event) { return event.startsAtMs < from || event.startsAtMs >= until; });
    return result;
}
}
