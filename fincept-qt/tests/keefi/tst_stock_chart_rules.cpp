#include "keefi/StockChartRules.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace keefi;
namespace {
qint64 ms(const QString& text) { return QDateTime::fromString(text, Qt::ISODate).toMSecsSinceEpoch(); }
AssetEvidence syntheticEvidence(qint64 at) { return {"합성 공개 검증 근거", "https://example.invalid/synthetic-only", "public:synthetic", 0, at, true}; }
StockChartInstrument instrument(QString country = "US") {
    StockChartInstrument result; result.symbol = country == "KR" ? "0101N0" : "SYN";
    result.marketCountry = country; result.identity = "stock:" + country + ':' + result.symbol;
    result.currency = country == "KR" ? "KRW" : "USD"; result.market = country == "KR" ? "KOSPI" : "NASDAQ";
    result.marketTimeZone = country == "KR" ? "Asia/Seoul" : "America/New_York"; result.nxtSupported = true; result.verified = true;
    return result;
}
QJsonObject session(const QString& from, const QString& to) { return {{"startTime", from}, {"endTime", to}}; }
QJsonObject usDay(QString date, bool holiday = false) {
    return {{"date", date}, {"dayMarket", QJsonValue::Null}, {"preMarket", QJsonValue::Null},
        {"regularMarket", holiday ? QJsonValue(QJsonValue::Null) : QJsonValue(session(date + "T13:30:00Z", date + "T20:00:00Z"))}, {"afterMarket", QJsonValue::Null}};
}
QJsonObject krDay(QString date, bool holiday = false) {
    return {{"date", date}, {"integrated", holiday ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonObject{
        {"preMarket", QJsonValue::Null}, {"regularMarket", session(date + "T09:00:00+09:00", date + "T15:30:00+09:00")},
        {"afterMarket", session(date + "T15:30:00+09:00", date + "T20:00:00+09:00")}})}};
}
HttpResult response(const QJsonObject& result) {
    HttpResult out; out.status = 200; out.body = QJsonDocument(QJsonObject{{"result", result}}).toJson(QJsonDocument::Compact); return out;
}
QJsonObject calendarObject(QString country = "US") {
    const auto day = [&](QString date) { return country == "KR" ? krDay(date) : usDay(date); };
    return {{"today", day("2026-10-23")}, {"previousBusinessDay", day("2026-10-22")}, {"nextBusinessDay", day("2026-10-26")}};
}
QJsonObject candle(QString time, QString close = "100", QString currency = "USD", QString volume = "1") {
    return {{"timestamp", time}, {"openPrice", close}, {"highPrice", close}, {"lowPrice", close},
        {"closePrice", close}, {"volume", volume}, {"currency", currency}};
}
StockCalendar calendar(QString country = "US") {
    StockCalendar result; result.available = true; result.marketCountry = country; result.evidence = syntheticEvidence(ms("2026-10-23T20:00:00Z"));
    for (const auto date : {"2026-10-22", "2026-10-23", "2026-10-26"}) {
        StockSessionDay day; day.tradingDate = QDate::fromString(date, Qt::ISODate); day.state = StockCalendarState::Sessions;
        const QString suffix = country == "KR" ? "T09:00:00+09:00" : "T13:30:00Z";
        const QString end = country == "KR" ? "T15:30:00+09:00" : "T20:00:00Z";
        day.sessions.append({StockSessionKind::Regular, ms(QString(date) + suffix), ms(QString(date) + end), {}, {}});
        if (country == "KR") day.sessions.append({StockSessionKind::After, ms(QString(date) + "T15:30:00+09:00"), ms(QString(date) + "T20:00:00+09:00"), {}, {}});
        result.days.insert(day.tradingDate, day);
    }
    result.nextBusinessDay.insert(QDate(2026,10,22), QDate(2026,10,23)); result.nextBusinessDay.insert(QDate(2026,10,23), QDate(2026,10,26)); return result;
}
StockCandlePage page(QString country, const QList<qint64>& starts, QString volume = "1") {
    StockCandlePage result; result.available = true; result.instrument = instrument(country); result.source = StockSourceKind::Toss;
    result.adjustment = StockAdjustment::SourceAdjusted; result.nativeIntervalMs = 60000;
    result.sourceUrl = "https://openapi.tossinvest.com/api/v1/candles";
    for (const auto start : starts) {
        const QDate date = QDateTime::fromMSecsSinceEpoch(start, Qt::UTC).date();
        result.candles.append({{start, 0, "100", "100", "100", "100", volume}, date, start + 60000});
    }
    if (!starts.isEmpty()) result.requestStartedAtMs = starts.last() + 60000;
    result.receivedAtMs = result.requestStartedAtMs + 1000; return result;
}
StockRestrictions completeRestrictions(const StockCandlePage& input) {
    StockRestrictions result; result.historicalCoverage = StockHistoryCoverage::VerifiedIntervals;
    result.evidence = syntheticEvidence(input.receivedAtMs);
    if (!input.candles.isEmpty()) result.intervals.append({input.candles.first().candle.startsAtMs,
        input.candles.last().sourceLabelAtMs, false, {}, {}, result.evidence});
    return result;
}
bool excludes(const ChartStudyTimelineRow& row, StockBarExclusion reason) { return row.exclusions.contains(reason); }
}

class StockChartRulesTest final : public QObject {
    Q_OBJECT
  private slots:
    void calendarSeparatesExplicitHolidayFromMissingFields_data();
    void calendarSeparatesExplicitHolidayFromMissingFields();
    void malformedCalendarRejectsAtomically_data();
    void malformedCalendarRejectsAtomically();
    void calendarUsesActualSessionInstantsForDstAndEarlyClose();
    void stockMinuteEndAndDailyLocalMidnightAreDistinct();
    void malformedCandlesRejectAtomically_data();
    void malformedCandlesRejectAtomically();
    void completionUsesRequestStartRatherThanReplyTime();
    void nonNxtClosingBarStaysPendingUntilPostDeadlineRestAndIsMixed();
    void nxtClosingMinuteAndUnknownNxtMetadataCannotPretendRegularOnly();
    void officialHolidayConnectsButIntradayAbsenceBreaks();
    void sessionAnchoredGroupsRequireAllChildrenAndRejectShortTail();
    void sourceZeroVolumeIsPreservedWithoutInventingVi();
    void unknownHistoricalRestrictionsNeverBecomeAnAllClear();
    void futureCoordinatesUseVerifiedTradingSessionsAndStopAtUnknownCoverage();
    void timelineOverloadAcceptsActualNonUtcCoordinatesAndCompletion();
    void timelineOverloadProjectsHistoricalPathsOntoActualTradingCoordinates();
    void timelineMismatchRejectsWithoutChangingLegacyCryptoCaller();
    void stockAdxAndCompressionUseNormalClosureAndResetOnIntradayGap();
    void stockAdxAndCompressionMatchExistingCryptoOnSameUtcRun();
    void stockTimelinePreservesPrefixSignalsAndExactWallClockHorizon_data();
    void stockTimelinePreservesPrefixSignalsAndExactWallClockHorizon();
};

void StockChartRulesTest::calendarSeparatesExplicitHolidayFromMissingFields_data() {
    QTest::addColumn<QString>("country"); QTest::addColumn<bool>("missing");
    QTest::newRow("kr-holiday") << QString("KR") << false; QTest::newRow("kr-missing") << QString("KR") << true;
    QTest::newRow("us-holiday") << QString("US") << false; QTest::newRow("us-missing") << QString("US") << true;
}
void StockChartRulesTest::calendarSeparatesExplicitHolidayFromMissingFields() {
    QFETCH(QString, country); QFETCH(bool, missing); auto object = calendarObject(country);
    auto today = country == "KR" ? krDay("2026-10-23", true) : usDay("2026-10-23", true);
    if (missing) today.remove(country == "KR" ? "integrated" : "regularMarket"); object["today"] = today;
    const auto result = parseStockCalendar(country, response(object), ms("2026-10-23T10:00:00Z"));
    QVERIFY(result.available); QVERIFY(result.days.contains(QDate(2026,10,23)));
    QCOMPARE(result.days[QDate(2026,10,23)].state, missing ? StockCalendarState::Unknown : StockCalendarState::ExplicitHoliday);
    QCOMPARE(result.evidence.sourceTimeMs, 0); QCOMPARE(result.evidence.receivedAtMs, ms("2026-10-23T10:00:00Z"));
}
void StockChartRulesTest::malformedCalendarRejectsAtomically_data() {
    QTest::addColumn<QString>("fault");
    for (const auto* name : {"unbound-country", "missing-offset", "reverse", "overlap", "auction-outside", "duplicate-date"}) QTest::newRow(name) << QString(name);
}
void StockChartRulesTest::malformedCalendarRejectsAtomically() {
    QFETCH(QString, fault); auto object = calendarObject(); auto today = object["today"].toObject();
    auto regular = today["regularMarket"].toObject();
    if (fault == "missing-offset") regular["startTime"] = "2026-10-23T13:30:00";
    if (fault == "reverse") regular["endTime"] = "2026-10-23T13:00:00Z";
    if (fault == "overlap") today["preMarket"] = session("2026-10-23T12:00:00Z", "2026-10-23T14:00:00Z");
    if (fault == "auction-outside") { object = calendarObject("KR"); today = object["today"].toObject(); auto integrated = today["integrated"].toObject();
        regular = integrated["regularMarket"].toObject(); regular["singlePriceAuctionStartTime"] = "2026-10-23T21:00:00+09:00";
        integrated["regularMarket"] = regular; today["integrated"] = integrated;
    } else today["regularMarket"] = regular;
    object["today"] = today;
    if (fault == "duplicate-date") { auto next = object["nextBusinessDay"].toObject(); next["date"] = "2026-10-23"; object["nextBusinessDay"] = next; }
    const auto result = parseStockCalendar(fault == "unbound-country" ? "ZZ" : fault == "auction-outside" ? "KR" : "US", response(object), ms("2026-10-23T21:00:00Z"));
    QVERIFY(!result.available); QVERIFY(!result.reason.isEmpty()); QVERIFY(result.days.isEmpty());
}
void StockChartRulesTest::calendarUsesActualSessionInstantsForDstAndEarlyClose() {
    auto object = calendarObject(); auto today = object["today"].toObject();
    today["regularMarket"] = session("2026-10-23T22:30:00+09:00", "2026-10-24T02:00:00+09:00"); object["today"] = today;
    auto next = object["nextBusinessDay"].toObject(); next["date"] = "2026-11-02";
    next["regularMarket"] = session("2026-11-02T23:30:00+09:00", "2026-11-03T06:00:00+09:00"); object["nextBusinessDay"] = next;
    const auto result = parseStockCalendar("US", response(object), ms("2026-10-23T21:00:00Z")); QVERIFY(result.available);
    QCOMPARE(result.days[QDate(2026,10,23)].sessions.first().fromMs, ms("2026-10-23T13:30:00Z"));
    QCOMPARE(result.days[QDate(2026,10,23)].sessions.first().toMs, ms("2026-10-23T17:00:00Z"));
    QCOMPARE(result.days[QDate(2026,11,2)].sessions.first().fromMs, ms("2026-11-02T14:30:00Z"));
}
void StockChartRulesTest::stockMinuteEndAndDailyLocalMidnightAreDistinct() {
    auto minute = response({{"candles", QJsonArray{candle("2026-10-23T13:32:00Z"), candle("2026-10-23T13:31:00Z")}}, {"nextBefore", "2026-10-23T13:31:00Z"}});
    const auto parsed = parseTossStockCandles(instrument(), 60000, StockAdjustment::SourceAdjusted, minute, ms("2026-10-23T13:33:00Z"), ms("2026-10-23T13:33:01Z"));
    QVERIFY(parsed.available); QCOMPARE(parsed.candles.size(), 2); QCOMPARE(parsed.candles.first().candle.startsAtMs, ms("2026-10-23T13:30:00Z"));
    QCOMPARE(parsed.candles.first().sourceLabelAtMs, ms("2026-10-23T13:31:00Z")); QCOMPARE(parsed.candles.first().candle.lastStoredTickMs, 0);
    QCOMPARE(parsed.nextBefore, QString("2026-10-23T13:31:00Z"));
    const auto overnight = parseTossStockCandles(instrument(), 60000, StockAdjustment::SourceAdjusted,
        response({{"candles", QJsonArray{candle("2026-10-23T00:01:00Z")}}, {"nextBefore", QJsonValue::Null}}), ms("2026-10-23T00:02:00Z"), ms("2026-10-23T00:02:01Z"));
    QVERIFY(overnight.available); // Synthetic New York Oct22 20:01 END belongs to the Oct23 trading day.
    QCOMPARE(overnight.candles.first().tradingDate, QDate(2026,10,23));
    QCOMPARE(overnight.candles.first().candle.startsAtMs, ms("2026-10-23T00:00:00Z"));
    const auto endsAtBoundary = parseTossStockCandles(instrument(), 60000, StockAdjustment::SourceAdjusted,
        response({{"candles", QJsonArray{candle("2026-10-23T00:00:00Z")}}, {"nextBefore", QJsonValue::Null}}), ms("2026-10-23T00:01:00Z"), ms("2026-10-23T00:01:01Z"));
    QVERIFY(endsAtBoundary.available); QCOMPARE(endsAtBoundary.candles.first().tradingDate, QDate(2026,10,22));
    const auto daily = parseTossStockCandles(instrument(), 86400000, StockAdjustment::SourceAdjusted,
        response({{"candles", QJsonArray{candle("2026-10-05T13:00:00+09:00")}}, {"nextBefore", QJsonValue::Null}}), ms("2026-10-07T00:00:00Z"), ms("2026-10-07T00:00:01Z"));
    QVERIFY(daily.available); QCOMPARE(daily.candles.first().tradingDate, QDate(2026,10,5));
    QCOMPARE(daily.candles.first().candle.startsAtMs, ms("2026-10-05T04:00:00Z")); QVERIFY(daily.candles.first().candle.startsAtMs % 86400000 != 0);
}
void StockChartRulesTest::malformedCandlesRejectAtomically_data() {
    QTest::addColumn<QString>("fault"); for (const auto* name : {"metadata", "currency", "price-newline", "negative-volume", "ohlc", "missing-offset", "duplicate", "ascending", "page201", "daily-not-midnight"}) QTest::newRow(name) << QString(name);
}
void StockChartRulesTest::malformedCandlesRejectAtomically() {
    QFETCH(QString, fault); auto metadata = instrument(); auto row = candle("2026-10-23T13:31:00Z");
    if (fault == "metadata") metadata.verified = false;
    if (fault == "currency") row["currency"] = "KRW";
    if (fault == "price-newline") row["closePrice"] = "100\n";
    if (fault == "negative-volume") row["volume"] = "-1";
    if (fault == "ohlc") row["highPrice"] = "90";
    if (fault == "missing-offset") row["timestamp"] = "2026-10-23T13:31:00";
    if (fault == "daily-not-midnight") row["timestamp"] = "2026-10-23T13:31:00Z";
    QJsonArray rows; rows.append(row);
    if (fault == "duplicate") rows.append(row);
    if (fault == "ascending") rows.append(candle("2026-10-23T13:32:00Z"));
    if (fault == "page201") for (int i = 0; i < 200; ++i) rows.append(row);
    const auto result = parseTossStockCandles(metadata, fault == "daily-not-midnight" ? 86400000 : 60000, StockAdjustment::SourceAdjusted,
        response({{"candles", rows}, {"nextBefore", QJsonValue::Null}}), ms("2026-10-23T21:00:00Z"), ms("2026-10-23T21:00:01Z"));
    QVERIFY(!result.available); QVERIFY(!result.reason.isEmpty()); QVERIFY(result.candles.isEmpty());
}
void StockChartRulesTest::completionUsesRequestStartRatherThanReplyTime() {
    auto input = page("US", {ms("2026-10-23T13:30:00Z")}); auto options = StockChartOptions{}; options.intervalMs = 60000;
    input.requestStartedAtMs = ms("2026-10-23T13:30:59.999Z"); input.receivedAtMs = ms("2026-10-23T13:31:05Z");
    const auto before = buildStockChart(input, calendar(), completeRestrictions(input), options);
    QVERIFY(before.series.candles.isEmpty()); QCOMPARE(before.pending.size(), 1); QCOMPARE(before.pending.first().completesAtMs, ms("2026-10-23T13:31:00Z"));
    input.requestStartedAtMs = ms("2026-10-23T13:31:00Z");
    const auto after = buildStockChart(input, calendar(), completeRestrictions(input), options);
    QCOMPARE(after.series.candles.size(), 1); QCOMPARE(after.timeline.rows.size(), 1); QCOMPARE(after.timeline.rows.first().completesAtMs, ms("2026-10-23T13:31:00Z"));
    QCOMPARE(after.series.sourceTimeMs, 0); QCOMPARE(after.series.receivedAtMs, input.receivedAtMs);
}
void StockChartRulesTest::nonNxtClosingBarStaysPendingUntilPostDeadlineRestAndIsMixed() {
    auto input = page("KR", {ms("2026-10-23T15:29:00+09:00")}); input.instrument.nxtSupported = false;
    StockChartOptions options; options.intervalMs = 60000; input.requestStartedAtMs = ms("2026-10-23T15:59:59+09:00"); input.receivedAtMs = ms("2026-10-23T16:00:03+09:00");
    const auto before = buildStockChart(input, calendar("KR"), completeRestrictions(input), options);
    QVERIFY(before.series.candles.isEmpty()); QCOMPARE(before.pending.size(), 1); QCOMPARE(before.pending.first().completesAtMs, ms("2026-10-23T16:00:00+09:00"));
    input.requestStartedAtMs = ms("2026-10-23T16:00:00+09:00");
    const auto after = buildStockChart(input, calendar("KR"), completeRestrictions(input), options);
    QCOMPARE(after.series.candles.size(), 1); QCOMPARE(after.timeline.rows.first().completesAtMs, ms("2026-10-23T16:00:00+09:00"));
    QVERIFY(!after.timeline.rows.first().eligible); QVERIFY(excludes(after.timeline.rows.first(), StockBarExclusion::MixedSession));
}
void StockChartRulesTest::nxtClosingMinuteAndUnknownNxtMetadataCannotPretendRegularOnly() {
    auto input = page("KR", {ms("2026-10-23T15:30:00+09:00")}); StockChartOptions options; options.intervalMs = 60000;
    const auto known = buildStockChart(input, calendar("KR"), completeRestrictions(input), options);
    QCOMPARE(known.series.candles.size(), 1); QVERIFY(!known.timeline.rows.first().eligible); QCOMPARE(known.timeline.rows.first().session, StockSessionKind::Mixed);
    input.instrument.nxtSupported.reset(); const auto unknown = buildStockChart(input, calendar("KR"), completeRestrictions(input), options);
    QVERIFY(unknown.series.candles.isEmpty()); QCOMPARE(unknown.pending.size(), 1); QVERIFY(!unknown.pending.first().reason.isEmpty());
}
void StockChartRulesTest::officialHolidayConnectsButIntradayAbsenceBreaks() {
    const auto friday = ms("2026-10-23T19:59:00Z"), monday = ms("2026-10-26T13:30:00Z");
    const auto input = page("US", {friday, monday, monday + 120000}); StockChartOptions options; options.intervalMs = 60000;
    const auto known = buildStockChart(input, calendar(), completeRestrictions(input), options);
    QCOMPARE(known.timeline.rows.size(), 3); QVERIFY(known.timeline.rows[1].connectsPrevious); QVERIFY(!known.timeline.rows[2].connectsPrevious);
    QCOMPARE(known.timeline.rows[2].missingSlotsBefore, std::optional<int>(1));
    auto unknownCalendar = calendar(); unknownCalendar.nextBusinessDay.clear();
    const auto unknown = buildStockChart(input, unknownCalendar, completeRestrictions(input), options);
    QVERIFY(!unknown.timeline.rows[1].connectsPrevious); QVERIFY(!unknown.timeline.rows[1].reason.isEmpty());
}
void StockChartRulesTest::sessionAnchoredGroupsRequireAllChildrenAndRejectShortTail() {
    QList<qint64> starts; const auto first = ms("2026-10-23T13:30:00Z"); for (int i = 0; i < 121; ++i) starts.append(first + i * 60000);
    auto input = page("US", starts); StockChartOptions options; options.intervalMs = 3600000;
    const auto full = buildStockChart(input, calendar(), completeRestrictions(input), options);
    QCOMPARE(full.series.candles.size(), 2); QCOMPARE(full.series.candles.first().startsAtMs, first);
    QCOMPARE(full.series.candles.first().volume, QString("60")); QCOMPARE(full.timeline.rows.first().completesAtMs, first + 3600000);
    input.candles.removeAt(30); const auto gap = buildStockChart(input, calendar(), completeRestrictions(input), options);
    QCOMPARE(gap.series.candles.size(), 1); QCOMPARE(gap.series.candles.first().startsAtMs, first + 3600000);
    auto shortCalendar = calendar(); shortCalendar.days[QDate(2026,10,23)].sessions.first().toMs = first + 90 * 60000;
    input = page("US", starts.mid(0,90)); const auto tail = buildStockChart(input, shortCalendar, completeRestrictions(input), options);
    QCOMPARE(tail.series.candles.size(), 1); QVERIFY(!tail.pending.isEmpty());
}
void StockChartRulesTest::sourceZeroVolumeIsPreservedWithoutInventingVi() {
    const auto input = page("KR", {ms("2026-10-23T10:00:00+09:00")}, "0"); StockChartOptions options; options.intervalMs = 60000;
    const auto result = buildStockChart(input, calendar("KR"), completeRestrictions(input), options);
    QCOMPARE(result.series.candles.size(), 1); QCOMPARE(result.series.candles.first().volume, QString("0"));
    QVERIFY(!result.restrictions.currentVI.has_value()); QVERIFY(!excludes(result.timeline.rows.first(), StockBarExclusion::VolatilityInterruption));
}
void StockChartRulesTest::unknownHistoricalRestrictionsNeverBecomeAnAllClear() {
    const auto input = page("US", {ms("2026-10-23T13:30:00Z"), ms("2026-10-23T13:31:00Z")}); StockChartOptions options; options.intervalMs = 60000;
    StockRestrictions currentOnly; currentOnly.currentVI = false; currentOnly.historicalCoverage = StockHistoryCoverage::CurrentOnly;
    currentOnly.upperLimit = "100"; currentOnly.currentObservationAtMs = input.requestStartedAtMs;
    const auto result = buildStockChart(input, calendar(), currentOnly, options);
    QCOMPARE(result.series.candles.size(), 2); QCOMPARE(result.restrictions.currentVI, std::optional<bool>(false));
    for (const auto& row : result.timeline.rows) { QVERIFY(!row.eligible); QVERIFY(excludes(row, StockBarExclusion::RestrictionCoverageUnknown)); QVERIFY(row.closeDisplayEligible()); QVERIFY(!excludes(row, StockBarExclusion::PriceLimit)); }
    for(const auto& malformed: QList<QPair<QString,QString>>{{"oops","55"},{"110","bad"},{"90","100"}}){
        auto restrictions=completeRestrictions(input);
        for(auto& range:restrictions.intervals){range.upperLimit=malformed.first;range.lowerLimit=malformed.second;}
        const auto invalid=buildStockChart(input,calendar(),restrictions,options);
        QCOMPARE(invalid.timeline.rows.size(),2);
        for(const auto& row:invalid.timeline.rows){
            QVERIFY(excludes(row,StockBarExclusion::InvalidPriceLimit));
            QVERIFY(!row.eligible && !row.closeDisplayEligible());
        }
    }
    QVERIFY(result.studies.entries.isEmpty()); QVERIFY(!result.studies.scorecard.available);
}
void StockChartRulesTest::futureCoordinatesUseVerifiedTradingSessionsAndStopAtUnknownCoverage() {
    const auto input = page("US", {ms("2026-10-23T19:59:00Z")}); StockChartOptions options; options.intervalMs = 60000;
    const auto result = buildStockChart(input, calendar(), completeRestrictions(input), options);
    QVERIFY(result.timeline.projectionCovered); QCOMPARE(result.timeline.nextStartsAtMs.size(), 20);
    QCOMPARE(result.timeline.nextStartsAtMs.first(), ms("2026-10-26T13:30:00Z")); QCOMPARE(result.timeline.nextStartsAtMs.last(), ms("2026-10-26T13:49:00Z"));
    auto unknown = calendar(); unknown.nextBusinessDay.clear();
    const auto limited = buildStockChart(input, unknown, completeRestrictions(input), options);
    QVERIFY(!limited.timeline.projectionCovered); QVERIFY(limited.timeline.nextStartsAtMs.isEmpty()); QVERIFY(!limited.timeline.projectionReason.isEmpty());
}
void StockChartRulesTest::timelineOverloadAcceptsActualNonUtcCoordinatesAndCompletion() {
    QList<MarketCandle> rows; ChartStudyTimeline timeline;
    for (int i = 0; i < 40; ++i) {
        const auto start = ms("2026-08-03T13:30:00Z") + qint64(i) * 86400000;
        rows.append({start, 0, "100", "101", "99", "100", "1"});
        ChartStudyTimelineRow state; state.startsAtMs = start; state.completesAtMs = start + 3600000; state.connectsPrevious = i > 0; state.eligible = true; timeline.rows.append(state);
    }
    timeline.adjustedComparable = true;
    const auto result = keefiChartStudies(rows, 3600000, timeline);
    QVERIFY(result.available); QCOMPARE(result.observations, 40); QCOMPARE(result.currentRunBars, 40);
    QCOMPARE(result.lastCompletedAtMs, timeline.rows.last().completesAtMs); QCOMPARE(result.volumes.size(), 40);
    QVERIFY(!result.cloud.isEmpty()); QCOMPARE(result.cloud.last().atMs, rows.last().startsAtMs); QCOMPARE(result.cloud.last().fast, 100.0);
    QVERIFY(!result.similarity.available); QVERIFY(!result.similarity.reason.isEmpty());
}
void StockChartRulesTest::timelineOverloadProjectsHistoricalPathsOntoActualTradingCoordinates() {
    QList<MarketCandle> rows; ChartStudyTimeline timeline; const auto base = ms("2026-01-05T14:30:00Z");
    for (int i = 0; i < 200; ++i) {
        const auto start = base + qint64(i) * 86400000; const auto close = QString::number(100 + ((i % 20) * 7) % 13);
        rows.append({start, 0, close, close, close, close, "1"}); ChartStudyTimelineRow state;
        state.startsAtMs = start; state.completesAtMs = start + 3600000; state.connectsPrevious = i > 0; state.eligible = true; timeline.rows.append(state);
    }
    timeline.adjustedComparable = true; timeline.projectionCovered = true;
    for (int i = 1; i <= 20; ++i) timeline.nextStartsAtMs.append(rows.last().startsAtMs + qint64(i + 2) * 86400000);
    const auto result = keefiChartStudies(rows, 3600000, timeline);
    QVERIFY(result.similarity.available); QCOMPARE(result.similarity.samples, 3); QCOMPARE(result.similarity.mean.size(), 21);
    QCOMPARE(result.similarity.mean[0].atMs, rows.last().startsAtMs);
    for (int i = 1; i <= 20; ++i) QCOMPARE(result.similarity.mean[i].atMs, timeline.nextStartsAtMs[i - 1]);
}
void StockChartRulesTest::timelineMismatchRejectsWithoutChangingLegacyCryptoCaller() {
    QList<MarketCandle> rows; for (int i = 0; i < 40; ++i) rows.append({60000LL * (i + 1), 0, "100", "101", "99", "100", "1"});
    const auto legacy = keefiChartStudies(rows, 60000); QVERIFY(legacy.available); QCOMPARE(legacy.currentRunBars, 40);
    ChartStudyTimeline missing; const auto invalid = keefiChartStudies(rows, 60000, missing); QVERIFY(!invalid.available); QVERIFY(!invalid.reason.isEmpty());
    const auto after = keefiChartStudies(rows, 60000); QCOMPARE(after.lastCompletedAtMs, legacy.lastCompletedAtMs);
    QCOMPARE(after.pullback.score, legacy.pullback.score); QCOMPARE(after.cloud.size(), legacy.cloud.size()); QCOMPARE(after.volumes.size(), legacy.volumes.size());
}
void StockChartRulesTest::stockAdxAndCompressionUseNormalClosureAndResetOnIntradayGap() {
    QList<MarketCandle> rows; ChartStudyTimeline timeline;
    for (int i = 0; i < 60; ++i) {
        const auto start = ms("2026-08-03T13:30:00Z") + qint64(i) * 86400000;
        const auto close = QString::number(100 + i); rows.append({start, 0, close, QString::number(102+i), QString::number(98+i), close, "1"});
        ChartStudyTimelineRow state; state.startsAtMs = start; state.completesAtMs = start + 3600000; state.connectsPrevious = i > 0; state.eligible = true; timeline.rows.append(state);
    }
    const auto studies = candleStudies(rows, 3600000, timeline); const auto compression = candleCompression(rows, 3600000, timeline);
    QVERIFY(studies.available); QVERIFY(studies.adxAvailable); QVERIFY(compression.available);
    timeline.rows[50].connectsPrevious = false;
    QVERIFY(!candleStudies(rows, 3600000, timeline).adxAvailable); QVERIFY(!candleCompression(rows, 3600000, timeline).available);
}
void StockChartRulesTest::stockAdxAndCompressionMatchExistingCryptoOnSameUtcRun() {
    QList<MarketCandle> rows; ChartStudyTimeline timeline;
    for (int i = 0; i < 60; ++i) {
        const qint64 start = qint64(i + 1) * 60000;
        rows.append({start, 0, "100", "102", "98", QString::number(100 + (i % 2)), "1"});
        ChartStudyTimelineRow state; state.startsAtMs = start; state.completesAtMs = start + 60000; state.connectsPrevious = i > 0; state.eligible = true; timeline.rows.append(state);
    }
    const auto cryptoStudies = candleStudies(rows, 60000); const auto stockStudies = candleStudies(rows, 60000, timeline);
    QVERIFY(cryptoStudies.available); QCOMPARE(stockStudies.available, cryptoStudies.available); QCOMPARE(stockStudies.adxAvailable, cryptoStudies.adxAvailable);
    QCOMPARE(stockStudies.adx, cryptoStudies.adx); QCOMPARE(stockStudies.plusDi, cryptoStudies.plusDi); QCOMPARE(stockStudies.minusDi, cryptoStudies.minusDi);
    const auto cryptoCompression = candleCompression(rows, 60000); const auto stockCompression = candleCompression(rows, 60000, timeline);
    QVERIFY(cryptoCompression.available); QVERIFY(stockCompression.available); QCOMPARE(stockCompression.bandLow, cryptoCompression.bandLow);
    QCOMPARE(stockCompression.bandHigh, cryptoCompression.bandHigh); QCOMPARE(stockCompression.channelLow, cryptoCompression.channelLow); QCOMPARE(stockCompression.channelHigh, cryptoCompression.channelHigh);
}
void StockChartRulesTest::stockTimelinePreservesPrefixSignalsAndExactWallClockHorizon_data() {
    QTest::addColumn<qint64>("interval"); QTest::addColumn<qint64>("horizon");
    QTest::newRow("1m") << qint64(60000) << qint64(3600000); QTest::newRow("5m") << qint64(300000) << qint64(3600000);
    QTest::newRow("15m") << qint64(900000) << qint64(14400000); QTest::newRow("1h") << qint64(3600000) << qint64(86400000);
    QTest::newRow("4h") << qint64(14400000) << qint64(86400000); QTest::newRow("1d") << qint64(86400000) << qint64(86400000);
}
void StockChartRulesTest::stockTimelinePreservesPrefixSignalsAndExactWallClockHorizon() {
    QFETCH(qint64, interval); QFETCH(qint64, horizon); const qint64 base = 1728000000000LL;
    const int target = 20 + int(horizon / interval); QList<MarketCandle> rows; ChartStudyTimeline timeline;
    timeline.adjustedComparable = true;
    for (int i = 0; i <= target; ++i) {
        const auto start = base + i * interval;
        rows.append({start,0,"100","101","99","100","0"}); ChartStudyTimelineRow state;
        state.startsAtMs=start; state.completesAtMs=start+interval; state.connectsPrevious=i>0; state.eligible=true; timeline.rows.append(state);
    }
    rows[10].low="90"; rows[11].high="120";
    rows[target].open=rows[target].close="110"; rows[target].high="111"; rows[target].low="109";
    const auto crypto=keefiChartStudies(rows,interval),stock=keefiChartStudies(rows,interval,timeline);
    QCOMPARE(crypto.entries.size(),1); QCOMPARE(stock.entries.size(),1);
    QCOMPARE(stock.entries.first().atMs,crypto.entries.first().atMs); QCOMPARE(stock.entries.first().waveLowAtMs,crypto.entries.first().waveLowAtMs);
    QCOMPARE(stock.entries.first().score,crypto.entries.first().score); QCOMPARE(stock.entries.first().entryClose,crypto.entries.first().entryClose);
    QVERIFY(stock.scorecard.available); QCOMPARE(stock.scorecard.horizonMs,horizon); QCOMPARE(stock.scorecard.samples,1);
    QCOMPARE(stock.scorecard.outcomes.first().evaluationAtMs,base+21*interval+horizon);
    QCOMPARE(stock.scorecard.meanReturnPercent,crypto.scorecard.meanReturnPercent);
    auto prefixTimeline=timeline; prefixTimeline.rows=timeline.rows.mid(0,21);
    const auto prefix=keefiChartStudies(rows.mid(0,21),interval,prefixTimeline); QCOMPARE(prefix.entries.size(),1);
    QCOMPARE(prefix.entries.first().atMs,stock.entries.first().atMs); QCOMPARE(prefix.entries.first().score,stock.entries.first().score);
    timeline.rows[target].completesAtMs+=1; const auto noExactObservation=keefiChartStudies(rows,interval,timeline);
    QVERIFY(!noExactObservation.scorecard.available); QCOMPARE(noExactObservation.scorecard.samples,0);
    QCOMPARE(noExactObservation.scorecard.unresolvedSignals,1); QVERIFY(noExactObservation.scorecard.outcomes.isEmpty());
}

QTEST_GUILESS_MAIN(StockChartRulesTest)
#include "tst_stock_chart_rules.moc"
