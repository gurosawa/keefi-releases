#include "keefi/OfficialCalendarView.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/TradingTypes.h"
#include "keefi/InfoButton.h"
#include "keefi/UsabilityPresentation.h"
#include <QDateTime>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QGridLayout>
#include <algorithm>
namespace keefi {
namespace {
QString scheduleInformation(){return QObject::tr("KST 기준 예정 일정 · 변경 가능 · 이전·실제·예상치 미제공 · 중요도 미제공\n공식 원천에 제공된 일정 중 오늘부터 30일 안의 가까운 8개를 표시합니다. 예정 시각이 지나도 실제 발표를 확인한 것은 아닙니다.");}
QString kstText(qint64 at){return at>0?QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm")+" KST":QObject::tr("원천 시각 미제공");}
// Wrapped labels may shrink below one word but must keep their height for the width.
QSizePolicy wrappedPolicy(){QSizePolicy policy(QSizePolicy::Ignored,QSizePolicy::Preferred);policy.setHeightForWidth(true);return policy;}
}
OfficialCalendarView::OfficialCalendarView(OfficialCalendar* calendar,QWidget* parent):QWidget(parent),calendar_(calendar){
    setObjectName("officialCalendarView");setFont(KeefiFonts::font(FontRole::Body15));auto* layout=new QVBoxLayout(this);layout->setContentsMargins(16,14,16,14);layout->setSpacing(10);
    auto* heading=new QHBoxLayout;auto* title=new QLabel(tr("공식 발표 일정 · 오늘부터 30일"),this);title->setObjectName("officialCalendarTitle");title->setTextFormat(Qt::PlainText);title->setWordWrap(true);title->setProperty("type","sectionHeading");heading->addWidget(title,1);
    info_=new InfoButton(this);info_->setObjectName("officialCalendarInfo");info_->setInformation(tr("공식 발표 일정"),scheduleInformation());heading->addWidget(info_);
    auto* reload=new QPushButton(tr("새로고침"),this);reload->setObjectName("officialCalendarReload");reload->setMinimumHeight(44);reload->setAccessibleName(tr("공식 일정 다시 조회"));connect(reload,&QPushButton::clicked,this,[this]{if(calendar_)calendar_->refresh();});heading->addWidget(reload);layout->addLayout(heading);
    status_=new QLabel(this);status_->setObjectName("officialCalendarStatus");status_->setWordWrap(true);status_->setTextFormat(Qt::PlainText);setThemeStyle(status_,"color:${text3};");layout->addWidget(status_);
    privateContext_=new QLabel(this);privateContext_->setObjectName("marketFlowCalendarPrivateContext");privateContext_->setWordWrap(true);privateContext_->setTextFormat(Qt::PlainText);privateContext_->hide();layout->addWidget(privateContext_);
    rows_=new QVBoxLayout;rows_->setSpacing(4);layout->addLayout(rows_);layout->addStretch(1);
    if(calendar_)connect(calendar_,&OfficialCalendar::changed,this,&OfficialCalendarView::refresh);
    auto* timer=new QTimer(this);timer->setInterval(60000);connect(timer,&QTimer::timeout,this,&OfficialCalendarView::refresh);timer->start();refresh();
}
OfficialCalendarView::~OfficialCalendarView(){if(calendar_){disconnect(calendar_,nullptr,this,nullptr);calendar_->setActive(false);}}
void OfficialCalendarView::showEvent(QShowEvent* e){QWidget::showEvent(e);if(calendar_)calendar_->setActive(true);refresh();}
void OfficialCalendarView::hideEvent(QHideEvent* e){QWidget::hideEvent(e);if(calendar_)calendar_->setActive(backgroundActive_);}
void OfficialCalendarView::setBackgroundActive(bool active){backgroundActive_=active;if(calendar_)calendar_->setActive(active||isVisible());}
bool OfficialCalendarView::settled()const{
    if(!calendar_)return true;if(!backgroundActive_&&!isVisible())return false;
    for(const auto source:{OfficialCalendarSource::Bls,OfficialCalendarSource::Bea,OfficialCalendarSource::Fomc,OfficialCalendarSource::Bok})if(calendar_->feed(source).loading)return false;
    return true;
}
void OfficialCalendarView::setMarketFlowMode(bool enabled){
    if(marketFlowMode_==enabled)return;marketFlowMode_=enabled;
    layout()->setContentsMargins(enabled?0:16,enabled?0:14,enabled?0:16,enabled?0:14);rows_->setSpacing(enabled?0:4);
    privateContext_->setFont(KeefiFonts::font(FontRole::Caption12));setThemeStyle(privateContext_,"QLabel{color:${text3};}");
    findChild<QLabel*>("officialCalendarTitle")->setText(enabled?usabilityText(UsabilityText::FlowCalendarTitle):tr("공식 발표 일정 · 오늘부터 30일"));
    findChild<QPushButton*>("officialCalendarReload")->setVisible(!enabled);status_->setVisible(!enabled);refresh();
}
void OfficialCalendarView::setPrivateContext(bool unlocked,const QString& coinShare,const QString& usShare){
    const auto valid=[](QString text){if(text=="<0.1%")return true;if(!text.endsWith('%'))return false;text.chop(1);for(const auto c:text)if((c<'0'||c>'9')&&c!='.')return false;const auto n=decimal::normalize(text);return !n.isEmpty()&&decimal::compare(n,"100")<=0;};
    const auto text=marketFlowMode_&&unlocked&&valid(coinShare)&&valid(usShare)?usabilityText(UsabilityText::FlowCalendarContext).arg(coinShare,usShare):QString{};
    privateContext_->setText(text);privateContext_->setAccessibleName(text);privateContext_->setToolTip({});privateContext_->setVisible(!text.isEmpty());
}
void OfficialCalendarView::refresh(){
    while(auto* item=rows_->takeAt(0)){delete item->widget();delete item;}
    if(!calendar_){status_->setText(tr("공개 일정 조회 연결 없음"));return;}
    const QTimeZone kst("Asia/Seoul");const auto now=QDateTime::currentMSecsSinceEpoch();QStringList statuses,sourceDetails;
    struct Item{OfficialCalendarEvent event;QString source;OfficialCalendarFeed feed;};QList<Item> items;
    for(auto source:{OfficialCalendarSource::Bls,OfficialCalendarSource::Bea,OfficialCalendarSource::Fomc,OfficialCalendarSource::Bok}){
        const auto feed=calendar_->feed(source);const auto name=OfficialCalendar::sourceName(source);
        if(!feed.available)statuses<<name+" · "+feed.reason;
        else{
            statuses<<name+tr(" · 수신 %1").arg(kstText(feed.receivedAtMs));
            if(feed.events.isEmpty())statuses<<tr("오늘부터 30일 안에 제공된 일정 없음 · 공식 일정 범위 확인 필요");
            for(const auto& event:feed.events)items.append({event,source==OfficialCalendarSource::Bls?QString("BLS"):source==OfficialCalendarSource::Bea?QString("BEA"):source==OfficialCalendarSource::Fomc?QString("FOMC"):tr("한은"),feed});
        }
        sourceDetails<<usabilitySourceTime(name,feed.sourceUpdatedAtMs);
    }
    status_->setText(usabilityPublicText(statuses.join('\n')));status_->setToolTip(sourceDetails.join(" · "));info_->setInformation(tr("공식 발표 일정"),scheduleInformation()+"\n\n"+sourceDetails.join("\n"));
    std::sort(items.begin(),items.end(),[](const auto& a,const auto& b){return a.event.startsAtMs<b.event.startsAtMs;});
    if(marketFlowMode_){
        // A date-only meeting stays listed through its whole KST day.
        items.erase(std::remove_if(items.begin(),items.end(),[now](const auto& item){return item.event.cancelled||(item.event.dateOnly?item.event.startsAtMs+86400000LL<=now:item.event.startsAtMs<now)||item.event.startsAtMs>now+7*86400000LL;}),items.end());
        info_->setInformation(usabilityText(UsabilityText::FlowCalendarTitle),usabilityText(UsabilityText::FlowCalendarInfo));
    }
    const auto dday=[&](qint64 at){const auto days=QDateTime::fromMSecsSinceEpoch(now,kst).date().daysTo(QDateTime::fromMSecsSinceEpoch(at,kst).date());return days==0?usabilityText(UsabilityText::FlowCalendarToday):usabilityText(UsabilityText::FlowCalendarDays).arg(days);};
    // One plain sentence for the market flow headline: day, part of day and the Korean release name.
    const auto dayPart=[](int hour){return usabilityText(hour<6?UsabilityText::FlowDawn:hour<12?UsabilityText::FlowMorning:hour<18?UsabilityText::FlowAfternoon:UsabilityText::FlowNight);};
    const auto weekday=[](const QDate& date,bool full){static const char* names[]={"월","화","수","목","금","토","일"};const auto name=QString::fromUtf8(names[qBound(1,date.dayOfWeek(),7)-1]);return full?name+QStringLiteral("요일"):name;};
    const auto releaseName=[](const QString& original){const auto korean=usabilityCalendarTitle(usabilityPublicText(original));return korean==usabilityPublicText(original)?usabilityText(UsabilityText::FlowCalendarRelease).arg(korean):korean;};
    QString summary;
    if(marketFlowMode_&&!items.isEmpty()){
        const auto first=QDateTime::fromMSecsSinceEpoch(items.first().event.startsAtMs,kst);const auto days=QDateTime::fromMSecsSinceEpoch(now,kst).date().daysTo(first.date());
        const auto day=days==0?usabilityText(UsabilityText::FlowDayToday):days==1?usabilityText(UsabilityText::FlowDayTomorrow):weekday(first.date(),true);
        const auto name=usabilityParticle(releaseName(items.first().event.title),QStringLiteral("이"),QStringLiteral("가"));
        summary=items.first().event.dateOnly?usabilityText(UsabilityText::FlowCalendarDateSentence).arg(day,name):usabilityText(UsabilityText::FlowCalendarSentence).arg(day,dayPart(first.time().hour()),name);
    }
    if(publicSummary_!=summary){publicSummary_=summary;emit publicSummaryChanged(publicSummary_);}
    if(marketFlowMode_&&items.isEmpty()){auto* empty=new QLabel(usabilityText(UsabilityText::FlowChecking),this);empty->setObjectName("marketFlowCalendarEmpty");rows_->addWidget(empty);}

    for(const auto& item:items.mid(0,marketFlowMode_?items.size():8)){
        const auto local=QDateTime::fromMSecsSinceEpoch(item.event.startsAtMs,kst);const auto delta=item.event.startsAtMs-now;QString state;
        const bool dateOnly=item.event.dateOnly;const auto dateOnlyText=usabilityText(UsabilityText::FlowCalendarDateOnly);
        // Date-only sources never get an invented clock time.
        const auto when=dateOnly?local.toString("MM/dd")+" · "+dateOnlyText:local.toString("MM/dd HH:mm")+" KST";
        const auto exact=dateOnly?local.toString("yyyy-MM-dd")+" · "+dateOnlyText:kstText(item.event.startsAtMs);
        if(dateOnly){const auto days=QDateTime::fromMSecsSinceEpoch(now,kst).date().daysTo(local.date());state=days<=0?tr("오늘"):tr("%1일 후").arg(days);}
        else if(delta<=0)state=tr("예정 시각 경과 · 발표 여부 미확인");else if(delta<3600000)state=tr("%1분 후").arg((delta+59999)/60000);else if(delta<86400000)state=tr("%1시간 후").arg((delta+3599999)/3600000);else state=tr("%1일 후").arg((delta+86399999)/86400000);
        const auto shownTitle=usabilityPublicText(item.event.title);
        QString meaning;
        if(marketFlowMode_){const auto lower=shownTitle.toLower();const auto kind=lower.contains("fomc")?UsabilityText::FlowCalendarFomc:shownTitle.contains(QStringLiteral("한국은행"))?UsabilityText::FlowCalendarBok:lower.contains("consumer price")||lower.contains("cpi")?UsabilityText::FlowCalendarCpi:lower.contains("employment")||lower.contains("payroll")?UsabilityText::FlowCalendarJobs:lower.contains("gross domestic")||lower.contains("gdp")?UsabilityText::FlowCalendarGdp:lower.contains("personal consumption")||lower.contains("personal income")||lower.contains("pce")?UsabilityText::FlowCalendarPce:UsabilityText::FlowCalendarOther;meaning=usabilityText(kind);}
        if(marketFlowMode_){
            const auto korean=usabilityCalendarTitle(shownTitle);const auto days=QDateTime::fromMSecsSinceEpoch(now,kst).date().daysTo(local.date());
            auto* container=new QWidget(this);container->setObjectName("officialCalendarEventRow");container->setAttribute(Qt::WA_StyledBackground,true);
            setThemeStyle(container,"QWidget#officialCalendarEventRow{border-top:1px solid ${rule};} QLabel{border:0;background:transparent;}");
            auto* grid=new QGridLayout(container);grid->setContentsMargins(0,12,0,12);grid->setHorizontalSpacing(14);grid->setVerticalSpacing(2);grid->setColumnMinimumWidth(0,58);grid->setColumnStretch(1,1);
            auto* date=new QLabel(local.toString("MM/dd"),container);date->setObjectName("officialCalendarEventDate");date->setFont(KeefiFonts::font(FontRole::Number11));grid->addWidget(date,0,0,Qt::AlignLeft|Qt::AlignTop);
            auto* time=new QLabel(KeefiFonts::mixedTextHtml(weekday(local.date(),false)+" "+(dateOnly?dateOnlyText:local.toString("HH:mm")),FontRole::Caption12),container);time->setObjectName("officialCalendarEventTime");setThemeStyle(time,"QLabel{color:${text3};}");grid->addWidget(time,1,0,Qt::AlignLeft|Qt::AlignTop);
            const auto summaryText=dday(item.event.startsAtMs)+" · "+when+" · "+item.source+" · "+korean;
            auto* title=new QLabel(korean,container);title->setObjectName("officialCalendarEventTitle");title->setTextFormat(Qt::PlainText);title->setWordWrap(true);title->setFont(KeefiFonts::font(FontRole::BodyBold15));
            title->setMinimumWidth(0);title->setSizePolicy(wrappedPolicy());title->setAccessibleName(summaryText+" · "+state);grid->addWidget(title,0,1);
            auto* meaningLabel=new QLabel(meaning,container);meaningLabel->setObjectName("officialCalendarEventMeaning");meaningLabel->setTextFormat(Qt::PlainText);meaningLabel->setWordWrap(true);meaningLabel->setFont(KeefiFonts::font(FontRole::Caption12));
            meaningLabel->setMinimumWidth(0);meaningLabel->setSizePolicy(wrappedPolicy());setThemeStyle(meaningLabel,"QLabel{color:${text2};}");grid->addWidget(meaningLabel,1,1);
            auto* ddayLabel=new QLabel(dday(item.event.startsAtMs),container);ddayLabel->setObjectName("officialCalendarEventDday");ddayLabel->setFont(KeefiFonts::font(FontRole::Number11));
            setThemeStyle(ddayLabel,days<=1?"QLabel{color:${accentText};}":"QLabel{color:${text2};}");grid->addWidget(ddayLabel,0,2,Qt::AlignRight|Qt::AlignTop);
            auto* info=new InfoButton(container);info->setObjectName("officialCalendarEventInfo");info->setInformation(korean,shownTitle+"\n"+exact+"\n"+meaning);grid->addWidget(info,1,2,Qt::AlignRight|Qt::AlignTop);
            rows_->addWidget(container);continue;
        }
        auto* container=new QWidget(this);container->setObjectName("officialCalendarEventRow");auto* row=new QHBoxLayout(container);row->setContentsMargins(0,0,0,0);row->setSpacing(6);
        auto* dot=new QLabel(QString(QChar(0x2022)),container);setThemeStyle(dot,"color:${text3};");dot->setAccessibleName(tr("공개 예정 일정 · 중요도 미제공"));row->addWidget(dot);
        const auto summary=(marketFlowMode_?dday(item.event.startsAtMs)+" · ":QString{})+when+" · "+item.source+" · "+shownTitle+(marketFlowMode_?"\n"+meaning:QString{});
        auto* title=new QLabel(summary,container);title->setObjectName("officialCalendarEventTitle");title->setWordWrap(marketFlowMode_);title->setTextFormat(Qt::PlainText);title->setMinimumWidth(0);title->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);title->setAccessibleName(summary+" · "+state);title->setToolTip(marketFlowMode_?QString{}:usabilitySourceTime(item.source,item.event.updatedAtMs));row->addWidget(title,1);
        const auto information=shownTitle+"\n"+exact+"\n"+state+tr("\n원문 시간대: %1\n원천 일정 갱신: %2\n원천 목록 갱신: %3\n수신: %4\n%5")
            .arg(item.event.sourceTimeZone,kstText(item.event.updatedAtMs),kstText(item.feed.sourceUpdatedAtMs),kstText(item.feed.receivedAtMs),scheduleInformation());
        auto* info=new InfoButton(container);info->setObjectName("officialCalendarEventInfo");info->setInformation(shownTitle,marketFlowMode_?shownTitle+"\n"+exact+"\n"+meaning:usabilityPublicText(information));row->addWidget(info);rows_->addWidget(container);
    }
    if(!marketFlowMode_&&items.size()>8){auto* count=new QLabel(tr("제공된 일정 %1개 중 가까운 8개 표시").arg(items.size()),this);count->setTextFormat(Qt::PlainText);rows_->addWidget(count);}
}
}
