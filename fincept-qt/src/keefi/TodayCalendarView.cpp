#include "keefi/PlainToolTip.h"
#include "keefi/TodayCalendarView.h"
#include "keefi/ProductIdentity.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/InfoButton.h"
#include "keefi/UsabilityPresentation.h"
#include <QDateTime>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QTimer>
#include <QTimeZone>
#include <QVBoxLayout>
#include <algorithm>

namespace keefi {
namespace {
const QTimeZone Kst("Asia/Seoul"), NewYork("America/New_York");
struct Row {
    QString title, country, importance, time, information;
    QDate date;
    qint64 at = 0;
    bool exact = false, past = false, earnings = false;
};
QString observed(qint64 at) {
    return at > 0 ? QDateTime::fromMSecsSinceEpoch(at, Kst).toString("yyyy-MM-dd HH:mm") + QStringLiteral(" KST") : QObject::tr("시각 미제공");
}
QString importance(const QString& title, OfficialCalendarSource source) {
    const auto value = title.toLower();
    if (source == OfficialCalendarSource::Fomc || source == OfficialCalendarSource::Bok || value.contains("consumer price") ||
        value.contains("cpi") || value.contains("employment situation") || value.contains("payroll") || value.contains(QStringLiteral("고용")))
        return QObject::tr("높음");
    if (value.contains("gross domestic") || value.contains("gdp") || value.contains("personal consumption") ||
        value.contains("personal income") || value.contains("pce")) return QObject::tr("보통");
    return QObject::tr("낮음");
}
QString policy() {
    return QObject::tr("중요도는 %1 기준입니다. 높음: FOMC·CPI·고용·한은 금통위, 보통: GDP·PCE·표시 대상 실적, 낮음: 그 밖의 일정. 가격 방향이나 매매 권유를 뜻하지 않습니다.\n"
        "‘발표됨’은 원천에 적힌 예정 시각 또는 날짜가 지났다는 표시입니다. 실제 발표나 발표 수치를 확인했다는 뜻이 아닙니다.\n"
        "정확한 시각이 있는 일정만 한국 시각과 ‘지금’ 선으로 비교합니다. 날짜만 있는 일정은 당일에 발표됨으로 처리하지 않습니다. 미국 실적의 날짜·장 전후는 미국 현지 기준입니다.\n"
        "경제 일정은 이번 주에 받은 지난 일정과 앞으로 7일을 표시합니다. 실적은 기존 원천이 제공하는 앞으로 7일만 표시하며, 이전 날짜를 새로 조회하거나 추정하지 않습니다.\n"
        "국내는 코스피 시총 상위 50·관심 회사의 실적 관련 IR, 미국은 시총 2,000억 달러 이상·관심 회사입니다. 이전·실제 경제 수치는 제공하지 않습니다.").arg(productName());
}
QLabel* label(const QString& text, FontRole role, QWidget* parent, const char* name) {
    auto* result = new QLabel(text, parent); result->setObjectName(name); result->setTextFormat(Qt::PlainText);
    result->setFont(KeefiFonts::font(role)); result->setMinimumWidth(0); return result;
}
void compactInfo(InfoButton* button) { button->setMinimumSize(0, 0); button->setFixedSize(22, 22); }
class CalendarEventLabel final : public QLabel {
  public:
    CalendarEventLabel(const QString& text,QWidget* parent) : QLabel(text,parent) {
        setObjectName("todayCalendarEventTitle");setTextFormat(Qt::PlainText);setFont(KeefiFonts::font(FontRole::Caption12));
        setMinimumWidth(0);setWordWrap(false);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);setToolTip(plainToolTip(text));
    }
  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);painter.setFont(font());painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(contentsRect(),alignment()|Qt::TextSingleLine,fontMetrics().elidedText(text(),Qt::ElideRight,contentsRect().width()));
    }
};
}

TodayCalendarView::TodayCalendarView(QWidget* parent, Clock clock) : QWidget(parent), clock_(clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); }) {
    setObjectName("todayCalendarView"); setAttribute(Qt::WA_StyledBackground, true);
    setMinimumSize(320, 240); setFont(KeefiFonts::font(FontRole::Caption12));
    setThemeStyle(this, "QWidget#todayCalendarView{background:${bg};border:1px solid ${rule};border-radius:12px;} QLabel{border:0;background:transparent;color:${text};}");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(14, 12, 14, 12); layout->setSpacing(0);
    auto* heading = new QHBoxLayout; heading->setSpacing(12);
    heading->addWidget(label(tr("다가오는 일정"), FontRole::Title17, this, "todayCalendarTitle"), 1);
    const QList<QPair<QString, QString>> filters{{"all", tr("전체")}, {"us", tr("미국")}, {"kr", tr("한국")}, {"earnings", tr("실적")}};
    for (const auto& [key, text] : filters) {
        auto* button = new QPushButton(text, this); button->setObjectName("todayCalendarFilter_" + key);
        button->setProperty("calendarFilter", key); button->setCheckable(true); button->setAutoExclusive(true); button->setChecked(key == filter_);
        button->setFont(KeefiFonts::font(FontRole::Caption12)); button->setFixedHeight(28); button->setMinimumWidth(36);
        setThemeStyle(button, "QPushButton{border:0;border-bottom:2px solid transparent;border-radius:0;padding:2px 4px;color:${text3};background:transparent;} QPushButton:checked{border-bottom:2px solid ${accent};color:${text};} QPushButton:hover{background:${surface};}");
        connect(button, &QPushButton::toggled, this, [this, key](bool selected) {
            if (!selected) return;
            filter_ = key; page_ = 0;
            for (auto* candidate : findChildren<QPushButton*>()) if (candidate->property("calendarFilter").isValid()) candidate->setChecked(candidate->property("calendarFilter").toString() == key);
            refresh();
        });
        heading->addWidget(button);
    }
    info_ = new InfoButton(this); info_->setObjectName("todayCalendarInfo"); compactInfo(info_); heading->addWidget(info_);
    layout->addLayout(heading); layout->addSpacing(6);
    status_ = label({}, FontRole::Caption12, this, "todayCalendarStatus"); status_->setFixedHeight(18);
    setThemeStyle(status_, "QLabel{color:${text3};}"); layout->addWidget(status_);
    rows_ = new QVBoxLayout; rows_->setContentsMargins(0, 0, 0, 0); rows_->setSpacing(0); layout->addLayout(rows_, 1);
    auto* footer = new QHBoxLayout; footer->setContentsMargins(0, 0, 0, 0); footer->setSpacing(8); footer->addStretch();
    previous_ = new QPushButton(tr("이전"), this); previous_->setObjectName("todayCalendarPrevious");
    next_ = new QPushButton(tr("다음"), this); next_->setObjectName("todayCalendarNext");
    for (auto* button : {previous_, next_}) {
        button->setFont(KeefiFonts::font(FontRole::Caption12)); button->setFixedSize(48, 24);
        setThemeStyle(button, "QPushButton{background:${bg};color:${text2};border:1px solid ${rule};border-radius:6px;padding:0;} QPushButton:hover{background:${surface};} QPushButton:disabled{color:${text3};}");
    }
    pageLabel_ = label({}, FontRole::Number11, this, "todayCalendarPage"); pageLabel_->setAlignment(Qt::AlignCenter); pageLabel_->setMinimumWidth(45);
    footer->addWidget(previous_); footer->addWidget(pageLabel_); footer->addWidget(next_); layout->addLayout(footer);
    connect(previous_, &QPushButton::clicked, this, [this] { page_ = qMax(0, page_ - 1); refresh(); });
    connect(next_, &QPushButton::clicked, this, [this] { ++page_; refresh(); });
    auto* timer = new QTimer(this); timer->setInterval(60000); connect(timer, &QTimer::timeout, this, &TodayCalendarView::refresh); timer->start();
    refresh();
}
void TodayCalendarView::setCalendar(OfficialCalendar* calendar) {
    if (calendar_ == calendar) return;
    if (calendar_) disconnect(calendar_, nullptr, this, nullptr);
    calendar_ = calendar;
    if (calendar_) connect(calendar_, &OfficialCalendar::changed, this, &TodayCalendarView::refresh);
    page_ = 0; refresh();
}
void TodayCalendarView::setEarnings(EarningsCalendar* calendar) {
    if (earnings_ == calendar) return;
    if (earnings_) disconnect(earnings_, nullptr, this, nullptr);
    earnings_ = calendar;
    if (earnings_) connect(earnings_, &EarningsCalendar::changed, this, &TodayCalendarView::refresh);
    page_ = 0; refresh();
}
void TodayCalendarView::setInterest(const QSet<QString>& koreaCodes, const QSet<QString>& usTickers) {
    QSet<QString> korea; for (const auto& code : koreaCodes) if (code.size() == 6) korea.insert(code.left(5));
    if (korea_ == korea && us_ == usTickers) return;
    korea_ = korea; us_ = usTickers; page_ = 0; refresh();
}
void TodayCalendarView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // Qt can deliver a resize while it is polishing/showing one of our rows.
    // Replacing those rows on that stack invalidates Qt's pending child access.
    if (event->size().height() != event->oldSize().height()) queueRefresh();
}
void TodayCalendarView::queueRefresh() {
    if (refreshQueued_) return;
    refreshQueued_ = true;
    QTimer::singleShot(0, this, [this] { refreshQueued_ = false; refresh(); });
}
void TodayCalendarView::refresh() {
    if (!rows_) return;
    if (refreshing_) { queueRefresh(); return; }
    QScopedValueRollback<bool> rendering(refreshing_, true);
    while (auto* item = rows_->takeAt(0)) { delete item->widget(); delete item; }
    const auto now = clock_(); const auto today = QDateTime::fromMSecsSinceEpoch(now, Kst).date();
    const auto firstDay = today.addDays(1 - today.dayOfWeek()), lastDay = today.addDays(7);
    QList<Row> exact, coarse; QStringList details; int economicFailed = 0, earningsFailed = 0; bool economicLoading = false, earningsLoading = false;
    auto accept = [&](Row row) {
        if (filter_ == "us" && row.country != "US") return;
        if (filter_ == "kr" && row.country != "KR") return;
        if (filter_ == "earnings" && !row.earnings) return;
        (row.exact ? exact : coarse).append(std::move(row));
    };
    if (calendar_) for (const auto source : {OfficialCalendarSource::Bls, OfficialCalendarSource::Bea, OfficialCalendarSource::Fomc, OfficialCalendarSource::Bok}) {
        const auto feed = calendar_->weekFeed(source, now); const auto name = OfficialCalendar::sourceName(source);
        economicLoading |= feed.loading; if (!feed.available && !feed.loading) ++economicFailed;
        details << name + QStringLiteral(" · ") + (feed.available ? tr("수신 %1").arg(observed(feed.receivedAtMs)) : feed.reason);
        for (const auto& event : feed.events) {
            const auto local = QDateTime::fromMSecsSinceEpoch(event.startsAtMs, Kst);
            if (event.cancelled || local.date() < firstDay || local.date() > lastDay) continue;
            Row row; row.country = source == OfficialCalendarSource::Bok ? "KR" : "US"; row.at = event.startsAtMs; row.date = local.date();
            row.title = usabilityCalendarTitle(usabilityPublicText(event.title)); row.importance = importance(event.title, source);
            row.exact = !event.dateOnly; row.time = row.exact ? local.toString("HH:mm") : tr("시각 미공개");
            row.past = row.exact ? event.startsAtMs <= now : row.date < today;
            row.information = name + "\n" + usabilityPublicText(event.title) + "\n" + (row.exact ? observed(event.startsAtMs) : row.date.toString(Qt::ISODate) + tr(" · 날짜만 공개")) +
                tr("\n원문 시간대: %1\n수신: %2\n\n%3").arg(event.sourceTimeZone, observed(feed.receivedAtMs), policy());
            accept(std::move(row));
        }
    } else ++economicFailed;
    if (earnings_) for (const auto source : {EarningsSource::Kind, EarningsSource::Nasdaq}) {
        const auto feed = earnings_->feed(source, now); const auto name = EarningsCalendar::sourceName(source);
        earningsLoading |= feed.loading; if (!feed.available && !feed.loading) ++earningsFailed;
        details << name + QStringLiteral(" · ") + (feed.available ? tr("수신 %1").arg(observed(feed.receivedAtMs)) : feed.reason);
        for (const auto& event : feed.events) {
            const bool korea = source == EarningsSource::Kind;
            const bool wanted = korea ? EarningsCalendar::earningsTitle(event.title) && korea_.contains(event.code) : event.marketCapUsd >= 200e9 || us_.contains(event.code);
            if (!wanted) continue;
            Row row; row.earnings = true; row.country = korea ? "KR" : "US"; row.date = event.date; row.at = event.startsAtMs;
            row.title = usabilityPublicText(korea ? event.name + tr(" 실적 설명회") : event.name + " " + event.code);
            row.importance = tr("보통"); row.exact = korea && event.timing == EarningsTiming::Scheduled;
            row.time = row.exact ? QDateTime::fromMSecsSinceEpoch(event.startsAtMs, Kst).toString("HH:mm") : event.timing == EarningsTiming::PreMarket ? tr("장 시작 전") : event.timing == EarningsTiming::AfterHours ? tr("장 마감 후") : tr("시각 미정");
            // Nasdaq startsAtMs only orders rows. It is never an actual event time.
            row.past = row.exact ? row.at <= now : row.date < QDateTime::fromMSecsSinceEpoch(now, korea ? Kst : NewYork).date();
            row.information = name + "\n" + usabilityPublicText(event.name + " · " + event.title) + "\n" + row.date.toString(Qt::ISODate) + " · " + row.time + (korea ? tr(" · 한국 시각") : tr(" · 미국 현지 날짜")) +
                tr("\n수신: %1").arg(observed(feed.receivedAtMs));
            if (!korea && !event.epsForecast.isEmpty()) row.information += tr("\n원천 예상 EPS: %1\n애널리스트 수: %2").arg(event.epsForecast, event.estimates.isEmpty() ? tr("미제공") : event.estimates);
            row.information += "\n\n" + policy(); accept(std::move(row));
        }
    } else ++earningsFailed;
    std::stable_sort(exact.begin(), exact.end(), [](const Row& a, const Row& b) { return a.at < b.at; });
    std::stable_sort(coarse.begin(), coarse.end(), [](const Row& a, const Row& b) { return a.date < b.date; });
    QStringList states;
    if (economicLoading) states << tr("경제 일정 받는 중"); else if (economicFailed) states << tr("경제 일정 일부를 받지 못했어요");
    if (earningsLoading) states << tr("실적 받는 중"); else if (earningsFailed) states << tr("실적 일부를 받지 못했어요");
    status_->setText(states.join(QStringLiteral(" · "))); status_->setVisible(!states.isEmpty());
    info_->setInformation(tr("다가오는 일정"), policy() + "\n\n" + details.join('\n'));
    const bool wide = height() >= 360; const int rowHeight = wide ? 40 : 32;
    const int available = height() - 24 - 28 - 6 - 24 - (states.isEmpty() ? 0 : 18) - (!exact.isEmpty() ? 16 : 0) - (!coarse.isEmpty() ? 18 : 0);
    const int limit = qBound(2, available / rowHeight, wide ? 7 : 5);
    const int coarseLimit = coarse.isEmpty() ? 0 : exact.isEmpty() ? limit : qMin(int(coarse.size()), qMax(qMin(2, limit - 1), limit - int(exact.size())));
    const int exactLimit = exact.isEmpty() ? 0 : limit - coarseLimit;
    const int pages = qMax(1, qMax(exactLimit ? int((exact.size() + exactLimit - 1) / exactLimit) : 0, coarseLimit ? int((coarse.size() + coarseLimit - 1) / coarseLimit) : 0));
    page_ = qBound(0, page_, pages - 1); pageLabel_->setText(QStringLiteral("%1 / %2").arg(page_ + 1).arg(pages));
    previous_->setEnabled(page_ > 0); next_->setEnabled(page_ + 1 < pages);
    auto appendDivider = [&](const QString& text, bool nowLine) {
        auto* divider = label(text, FontRole::Caption12, this, nowLine ? "todayCalendarNow" : "todayCalendarDateOnly");
        divider->setFixedHeight(nowLine ? 16 : 18);
        setThemeStyle(divider, nowLine ? "QLabel{color:${accentText};border-top:1px solid ${accent};}" : "QLabel{color:${text3};border-top:1px solid ${rule};}");
        rows_->addWidget(divider);
    };
    const int dateWidth=QFontMetrics(KeefiFonts::font(FontRole::Number11)).horizontalAdvance(QStringLiteral("00/00"));
    int timeWidth=0;
    const auto measureTime=[&](const QList<Row>& values){for(const auto& value:values)timeWidth=qMax(timeWidth,QFontMetrics(KeefiFonts::font(value.exact?FontRole::Number11:FontRole::Caption12)).horizontalAdvance(value.time));};
    measureTime(exact);measureTime(coarse);
    auto appendRow = [&](const Row& value) {
        auto* row = new QWidget(this); row->setObjectName("todayCalendarRow"); row->setProperty("country", value.country); row->setProperty("earnings", value.earnings); row->setProperty("exactTime", value.exact);
        row->setFixedHeight(rowHeight); row->setAttribute(Qt::WA_StyledBackground, true);
        setThemeStyle(row, "QWidget#todayCalendarRow{border-bottom:1px solid ${rule};} QLabel{border:0;background:transparent;}");
        auto* line = new QHBoxLayout(row); line->setContentsMargins(0, 0, 0, 0); line->setSpacing(10);
        auto* when = new QWidget(row); when->setFixedWidth(dateWidth+6+timeWidth); auto* whenLayout = new QHBoxLayout(when); whenLayout->setContentsMargins(0, 0, 0, 0); whenLayout->setSpacing(6);
        auto* date=label(value.date.toString("MM/dd"),FontRole::Number11,when,"todayCalendarDate");date->setFixedWidth(dateWidth);whenLayout->addWidget(date);
        auto* time = label(value.time, value.exact ? FontRole::Number11 : FontRole::Caption12, when, "todayCalendarTime");time->setFixedWidth(timeWidth);setThemeStyle(time, "QLabel{color:${text3};}"); whenLayout->addWidget(time); line->addWidget(when);
        auto* country = label(value.country, FontRole::Number11, row, "todayCalendarCountry"); country->setFixedWidth(28); line->addWidget(country);
        auto* title = new CalendarEventLabel(value.title,row);
        title->setAccessibleName(value.date.toString(Qt::ISODate) + " · " + value.time + " · " + value.country + " · " + value.title); line->addWidget(title, 1);
        auto* weight = label(value.importance, FontRole::Caption12, row, "todayCalendarImportance"); weight->setFixedWidth(45);
        setThemeStyle(weight, value.importance == tr("높음") ? "QLabel{color:${accentText};}" : "QLabel{color:${text3};}"); line->addWidget(weight);
        auto* state = label(value.past ? tr("발표됨") : value.earnings ? tr("실적") : tr("예정"), FontRole::Caption12, row, "todayCalendarEventState"); state->setFixedWidth(46); setThemeStyle(state, "QLabel{color:${text3};}"); line->addWidget(state);
        auto* info = new InfoButton(row); info->setObjectName("todayCalendarEventInfo"); compactInfo(info); info->setInformation(value.title, value.information); line->addWidget(info); rows_->addWidget(row);
    };
    const auto exactPage = exact.mid(page_ * exactLimit, exactLimit); bool nowShown = false;
    for (const auto& row : exactPage) { if (!row.past && !nowShown) { appendDivider(tr("지금"), true); nowShown = true; } appendRow(row); }
    if (!exactPage.isEmpty() && !nowShown) appendDivider(tr("지금"), true);
    const auto coarsePage = coarse.mid(page_ * coarseLimit, coarseLimit);
    if (!coarsePage.isEmpty()) { appendDivider(tr("날짜·시간대만 정해진 일정 · 미국 실적은 현지 날짜"), false); for (const auto& row : coarsePage) appendRow(row); }
    if (exact.isEmpty() && coarse.isEmpty()) {
        auto* empty = label(economicLoading || earningsLoading ? tr("일정을 받는 중이에요") : tr("이 조건에 해당하는 일정이 없어요"), FontRole::Caption12, this, "todayCalendarEmpty");
        empty->setAlignment(Qt::AlignCenter); setThemeStyle(empty, "QLabel{color:${text3};}"); rows_->addWidget(empty, 1);
    } else rows_->addStretch();
}
} // namespace keefi
