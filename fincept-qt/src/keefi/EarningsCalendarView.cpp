#include "keefi/EarningsCalendarView.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/InfoButton.h"
#include "keefi/UsabilityPresentation.h"
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QShowEvent>
#include <QTimer>
#include <QTimeZone>
#include <QVBoxLayout>
#include <algorithm>
namespace keefi {
namespace {
// Wrapped labels may shrink below one word but must keep their height for the width.
QSizePolicy wrappedPolicy() { QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred); policy.setHeightForWidth(true); return policy; }
QString weekday(const QDate& date) {
    static const char* names[] = {"월", "화", "수", "목", "금", "토", "일"};
    return QString::fromUtf8(names[qBound(1, date.dayOfWeek(), 7) - 1]);
}
}
EarningsCalendarView::EarningsCalendarView(EarningsCalendar* calendar, QWidget* parent) : QWidget(parent), calendar_(calendar) {
    setObjectName("earningsCalendarView"); setFont(KeefiFonts::font(FontRole::Body15));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(10);
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(usabilityText(UsabilityText::FlowEarningsTitle), this); title->setObjectName("earningsCalendarTitle");
    title->setTextFormat(Qt::PlainText); title->setWordWrap(true); title->setProperty("type", "sectionHeading"); heading->addWidget(title, 1);
    info_ = new InfoButton(this); info_->setObjectName("earningsCalendarInfo");
    info_->setInformation(usabilityText(UsabilityText::FlowEarningsTitle), usabilityText(UsabilityText::FlowEarningsInfo)); heading->addWidget(info_);
    layout->addLayout(heading);
    rows_ = new QVBoxLayout; rows_->setSpacing(0); layout->addLayout(rows_); layout->addStretch(1);
    if (calendar_) connect(calendar_, &EarningsCalendar::changed, this, &EarningsCalendarView::refresh);
    auto* timer = new QTimer(this); timer->setInterval(60000); connect(timer, &QTimer::timeout, this, &EarningsCalendarView::refresh); timer->start();
    refresh();
}
EarningsCalendarView::~EarningsCalendarView() { if (calendar_) { disconnect(calendar_, nullptr, this, nullptr); calendar_->setActive(false); } }
void EarningsCalendarView::showEvent(QShowEvent* e) { QWidget::showEvent(e); if (calendar_) calendar_->setActive(true); refresh(); }
void EarningsCalendarView::hideEvent(QHideEvent* e) { QWidget::hideEvent(e); if (calendar_) calendar_->setActive(backgroundActive_); }
void EarningsCalendarView::setBackgroundActive(bool active) { backgroundActive_ = active; if (calendar_) calendar_->setActive(active || isVisible()); }
bool EarningsCalendarView::settled() const {
    if (!calendar_) return true;
    if (!backgroundActive_ && !isVisible()) return false;
    return !calendar_->feed(EarningsSource::Kind).loading && !calendar_->feed(EarningsSource::Nasdaq).loading;
}
void EarningsCalendarView::setInterest(const QSet<QString>& koreaCodes, const QSet<QString>& usTickers) {
    // KIND names issuers by the first five characters of the six-character KRX code.
    QSet<QString> korea; for (const auto& code : koreaCodes) if (code.size() == 6) korea.insert(code.left(5));
    if (korea == korea_ && usTickers == us_) return;
    korea_ = korea; us_ = usTickers; refresh();
}
void EarningsCalendarView::refresh() {
    while (auto* item = rows_->takeAt(0)) { delete item->widget(); delete item; }
    const auto addNote = [&](const QString& text, const char* name) {
        auto* note = new QLabel(text, this); note->setObjectName(name); note->setTextFormat(Qt::PlainText); note->setWordWrap(true);
        note->setFont(KeefiFonts::font(FontRole::Caption12)); setThemeStyle(note, "QLabel{color:${text3};}"); rows_->addWidget(note);
    };
    if (!calendar_) { addNote(usabilityText(UsabilityText::FlowEarningsUnavailable), "earningsCalendarEmpty"); return; }
    const auto now = QDateTime::currentMSecsSinceEpoch(); const QTimeZone kst("Asia/Seoul"), newYork("America/New_York");
    QList<EarningsEvent> shown; QStringList statuses; bool loading = false, available = false;
    for (auto source : {EarningsSource::Kind, EarningsSource::Nasdaq}) {
        const auto feed = calendar_->feed(source); loading = loading || feed.loading; available = available || feed.available;
        statuses << EarningsCalendar::sourceName(source) + " · " + (feed.available ? tr("수신 %1").arg(QDateTime::fromMSecsSinceEpoch(feed.receivedAtMs, kst).toString("MM/dd HH:mm")) : feed.reason);
        for (const auto& event : feed.events) {
            const bool wanted = source == EarningsSource::Kind ? EarningsCalendar::earningsTitle(event.title) && korea_.contains(event.code)
                                                               : event.marketCapUsd >= LargeCapUsd || us_.contains(event.code);
            if (wanted) shown.append(event);
        }
    }
    info_->setInformation(usabilityText(UsabilityText::FlowEarningsTitle), usabilityText(UsabilityText::FlowEarningsInfo) + "\n\n" + usabilityPublicText(statuses.join('\n')));
    std::stable_sort(shown.begin(), shown.end(), [](const auto& a, const auto& b) { return a.startsAtMs < b.startsAtMs; });
    if (shown.isEmpty()) {
        addNote(loading ? usabilityText(UsabilityText::FlowChecking) : available ? usabilityText(UsabilityText::FlowEarningsEmpty) : usabilityText(UsabilityText::FlowEarningsUnavailable), "earningsCalendarEmpty");
        return;
    }
    for (const auto& event : shown.mid(0, MaximumRows)) {
        const bool korea = event.source == EarningsSource::Kind;
        const auto days = QDateTime::fromMSecsSinceEpoch(now, korea ? kst : newYork).date().daysTo(event.date);
        const auto dday = days <= 0 ? usabilityText(UsabilityText::FlowCalendarToday) : usabilityText(UsabilityText::FlowCalendarDays).arg(days);
        // Korean sessions carry a KST start time; US dates are New York dates with only open/close markers.
        const auto timeText = korea ? (event.timing == EarningsTiming::Scheduled ? QDateTime::fromMSecsSinceEpoch(event.startsAtMs, kst).toString("HH:mm") : usabilityText(UsabilityText::FlowEarningsTimeUnknown))
                                    : usabilityText(UsabilityText::FlowEarningsUs);
        const auto timing = event.timing == EarningsTiming::PreMarket ? usabilityText(UsabilityText::FlowEarningsPreMarket)
            : event.timing == EarningsTiming::AfterHours ? usabilityText(UsabilityText::FlowEarningsAfterHours) : usabilityText(UsabilityText::FlowEarningsTimeUnknown);
        const auto meaning = korea ? usabilityPublicText(event.title)
            : timing + (event.epsForecast.isEmpty() ? QString{} : QStringLiteral(" · ") + usabilityText(UsabilityText::FlowEarningsEps).arg(event.epsForecast, event.estimates.isEmpty() ? QStringLiteral("-") : event.estimates));
        const auto name = usabilityPublicText(korea ? event.name : event.name + QStringLiteral(" ") + event.code);
        const auto market = usabilityText(korea ? UsabilityText::FlowEarningsKorea : UsabilityText::FlowEarningsUs);
        auto* container = new QWidget(this); container->setObjectName("earningsEventRow"); container->setAttribute(Qt::WA_StyledBackground, true);
        setThemeStyle(container, "QWidget#earningsEventRow{border-top:1px solid ${rule};} QLabel{border:0;background:transparent;}");
        auto* grid = new QGridLayout(container); grid->setContentsMargins(0, 12, 0, 12); grid->setHorizontalSpacing(14); grid->setVerticalSpacing(2);
        grid->setColumnMinimumWidth(0, 58); grid->setColumnStretch(1, 1);
        auto* date = new QLabel(event.date.toString("MM/dd"), container); date->setObjectName("earningsEventDate"); date->setFont(KeefiFonts::font(FontRole::Number11));
        grid->addWidget(date, 0, 0, Qt::AlignLeft | Qt::AlignTop);
        auto* time = new QLabel(KeefiFonts::mixedTextHtml(weekday(event.date) + " " + timeText, FontRole::Caption12), container); time->setObjectName("earningsEventTime");
        setThemeStyle(time, "QLabel{color:${text3};}"); grid->addWidget(time, 1, 0, Qt::AlignLeft | Qt::AlignTop);
        auto* title = new QLabel(name, container); title->setObjectName("earningsEventTitle"); title->setTextFormat(Qt::PlainText); title->setWordWrap(true);
        title->setFont(KeefiFonts::font(FontRole::BodyBold15)); title->setMinimumWidth(0); title->setSizePolicy(wrappedPolicy());
        title->setAccessibleName(dday + " · " + event.date.toString("MM/dd") + " " + timeText + " · " + market + " · " + name + " · " + meaning); grid->addWidget(title, 0, 1);
        auto* meaningLabel = new QLabel(meaning, container); meaningLabel->setObjectName("earningsEventMeaning"); meaningLabel->setTextFormat(Qt::PlainText); meaningLabel->setWordWrap(true);
        meaningLabel->setFont(KeefiFonts::font(FontRole::Caption12)); meaningLabel->setMinimumWidth(0); meaningLabel->setSizePolicy(wrappedPolicy());
        setThemeStyle(meaningLabel, "QLabel{color:${text2};}"); grid->addWidget(meaningLabel, 1, 1);
        auto* ddayLabel = new QLabel(dday, container); ddayLabel->setObjectName("earningsEventDday"); ddayLabel->setFont(KeefiFonts::font(FontRole::Number11));
        setThemeStyle(ddayLabel, days <= 1 ? "QLabel{color:${accentText};}" : "QLabel{color:${text2};}"); grid->addWidget(ddayLabel, 0, 2, Qt::AlignRight | Qt::AlignTop);
        auto* marketLabel = new QLabel(market, container); marketLabel->setObjectName("earningsEventMarket"); marketLabel->setFont(KeefiFonts::font(FontRole::Caption12));
        setThemeStyle(marketLabel, "QLabel{color:${text3};}"); grid->addWidget(marketLabel, 1, 2, Qt::AlignRight | Qt::AlignTop);
        rows_->addWidget(container);
    }
    if (shown.size() > MaximumRows) addNote(usabilityText(UsabilityText::FlowEarningsMore).arg(shown.size() - MaximumRows), "earningsCalendarMore");
}
} // namespace keefi
