#include "keefi/OfficialNoticesView.h"
#include "keefi/KeefiTheme.h"
#include "keefi/InfoButton.h"
#include "keefi/KeefiFonts.h"
#include "keefi/UsabilityPresentation.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
namespace keefi {
namespace {
QLabel* plainLabel(const QString& text, QWidget* parent, FontRole role=FontRole::Body15) {
    auto* label = new QLabel(KeefiFonts::mixedTextHtml(text,role), parent);label->setFont(KeefiFonts::font(role));
    label->setTextFormat(Qt::AutoText);label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse); return label;
}
QString receivedText(qint64 at){return at>0?QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).toString("MM/dd HH:mm")+" KST · UTC+9":QObject::tr("아직 수신하지 않음");}
}
OfficialNoticesView::OfficialNoticesView(OfficialNotices* model, QWidget* parent) : QWidget(parent), notices_(model) {
    setObjectName("officialNoticesView");setFont(KeefiFonts::font(FontRole::Body15));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(16,14,16,14); layout->setSpacing(10);
    auto* header = new QHBoxLayout;
    auto* title = plainLabel(tr("거래소 공식 공지 · 원문 참고"),this,FontRole::Title17);title->setObjectName("officialNoticesTitle");title->setProperty("type","sectionHeading");header->addWidget(title,1);
    auto* info=new InfoButton(this);info->setObjectName("officialNoticesInfo");
    info->setInformation(tr("거래소 공식 공지"),tr("자동 보유 매칭 미지원 · 공지 목록만으로 내 코인 위험 없음 판정 불가\n공지 API에 구조화된 자산·체인 식별자가 없어 제목의 심볼로 보유 자산을 연결하지 않습니다. Phantom Mint 매핑도 미지원입니다.\n원천별 최신 20건 중 5건 표시 · 모든 공지·수정 사항을 포괄하지 않습니다.\nOKX: 약 5분 지연 가능 · 요청 IP 지역에 따른 목록"));header->addWidget(info);
    auto* reload=new QPushButton(tr("새로고침"),this);reload->setObjectName("officialNoticesReload");reload->setMinimumHeight(44);reload->setAccessibleName(tr("공식 공지 다시 조회"));
    connect(reload,&QPushButton::clicked,this,[this]{if(notices_)notices_->refresh();});header->addWidget(reload);layout->addLayout(header);
    status_=plainLabel({},this,FontRole::Caption12);status_->setObjectName("officialNoticesStatus");setThemeStyle(status_,"color:${text3};");layout->addWidget(status_);
    rows_=new QVBoxLayout;rows_->setSpacing(4);layout->addLayout(rows_);layout->addStretch(1);
    if(notices_)connect(notices_,&OfficialNotices::changed,this,&OfficialNoticesView::refresh);
    auto* timer=new QTimer(this);timer->setInterval(60000);connect(timer,&QTimer::timeout,this,&OfficialNoticesView::refresh);timer->start();
    bindThemePresentation(this,[this]{refresh();});
}
OfficialNoticesView::~OfficialNoticesView(){if(notices_){disconnect(notices_,nullptr,this,nullptr);notices_->setActive(false);}}
void OfficialNoticesView::setMarketFlowMode(bool enabled){
    if(marketFlowMode_==enabled)return;marketFlowMode_=enabled;
    layout()->setContentsMargins(enabled?0:16,enabled?0:14,enabled?0:16,enabled?0:14);rows_->setSpacing(enabled?0:4);
    findChild<QLabel*>("officialNoticesTitle")->setText(KeefiFonts::mixedTextHtml(enabled?usabilityText(UsabilityText::FlowNoticesTitle):tr("거래소 공식 공지 · 원문 참고"),FontRole::Title17));
    findChild<QPushButton*>("officialNoticesReload")->setVisible(!enabled);status_->setVisible(!enabled);if(enabled)findChild<InfoButton*>("officialNoticesInfo")->setInformation(usabilityText(UsabilityText::FlowNoticesTitle),usabilityText(UsabilityText::FlowNoticesInfo));refresh();
}
void OfficialNoticesView::refresh(){
    while(auto* item=rows_->takeAt(0)){delete item->widget();delete item;}
    if(!notices_){status_->setText(KeefiFonts::mixedTextHtml(tr("공개 공지 조회 연결 없음"),FontRole::Caption12));return;}
    const QTimeZone kst("Asia/Seoul");QStringList statuses,sources;
    if(marketFlowMode_){
        struct Item{OfficialNotice notice;OfficialNoticeSource source;QDate date;};QList<Item> items;
        for(const auto source:{OfficialNoticeSource::Okx,OfficialNoticeSource::Bithumb,OfficialNoticeSource::Upbit})for(const auto& notice:notices_->feed(source).notices){
            // Public event classification only: never compare these words to a holding or account.
            const auto category=notice.categories.join(' ').toLower(),title=notice.title.toLower();
            const QStringList tokens{"delist","caution","거래지원종료","거래유의","입출금중단","입출금일시중단","입출금일시중지","입금중단","출금중단","입금일시중단","출금일시중단","네트워크업그레이드","거래정지"};
            auto words=category+" "+title;words.remove(' ');bool relevant=false;for(const auto& token:tokens)if(words.contains(token)){relevant=true;break;}
            const bool interruption=title.contains("suspend")||title.contains("suspension")||title.contains("halt");
            relevant|=interruption&&(title.contains("deposit")||title.contains("withdraw"));
            relevant|=(title.contains("network")&&title.contains("upgrade"))||(category.contains("network")&&category.contains("upgrade"));if(!relevant)continue;
            // Timezone-free Bithumb dates stay dates; no synthetic UTC timestamp is created.
            const auto date=notice.publishedAtMs>0?QDateTime::fromMSecsSinceEpoch(notice.publishedAtMs,kst).date():QDate::fromString(notice.publishedText.left(10),"yyyy-MM-dd");
            items.append({notice,source,date});
        }
        std::stable_sort(items.begin(),items.end(),[](const auto& a,const auto& b){if(a.date!=b.date)return a.date>b.date;const bool aTimed=a.notice.publishedAtMs>0,bTimed=b.notice.publishedAtMs>0;if(aTimed!=bTimed)return aTimed;if(aTimed)return a.notice.publishedAtMs>b.notice.publishedAtMs;return false;});
        for(const auto& item:items.mid(0,5)){
            const auto name=OfficialNotices::sourceName(item.source);const auto shownTitle=usabilityPublicText(item.notice.title);
            const auto summary=shownTitle+" · "+name+(item.date.isValid()?" · "+item.date.toString(Qt::ISODate):QString{});
            // Display grouping of the same public words used to select the row; never a holding match.
            auto words=(item.notice.categories.join(' ')+" "+item.notice.title).toLower();words.remove(' ');
            const bool network=words.contains("network")&&words.contains("upgrade")||words.contains("네트워크업그레이드");
            const bool transfer=words.contains("입출금")||words.contains("입금")||words.contains("출금")||words.contains("deposit")||words.contains("withdraw");
            const auto category=transfer?UsabilityText::FlowNoticeWithdrawal:network?UsabilityText::FlowNoticeNetwork:UsabilityText::FlowNoticeDelisting;
            auto* container=new QWidget(this);container->setObjectName("officialNoticeRow");container->setAttribute(Qt::WA_StyledBackground,true);
            setThemeStyle(container,"QWidget#officialNoticeRow{border-top:1px solid ${rule};} QLabel{border:0;background:transparent;}");
            auto* column=new QVBoxLayout(container);column->setContentsMargins(0,12,0,12);column->setSpacing(2);
            const auto& c=themeColors();const auto strong=category!=UsabilityText::FlowNoticeNetwork;
            const auto font=[](FontRole role){const auto f=KeefiFonts::font(role);return QString("font-family:'%1';font-size:%2px;font-weight:%3;").arg(f.family().toHtmlEscaped()).arg(f.pixelSize()).arg(f.weight());};
            const auto meta=QString("<span style=\"%1color:%2;\">%3</span><span style=\"%4color:%5;\"> · %6 · </span><span style=\"%7color:%5;\">%8</span>")
                .arg(font(FontRole::CaptionStrong12),(strong?c.accentText:c.text3).name(),usabilityText(category).toHtmlEscaped(),font(FontRole::Caption12),c.text3.name(),name.toHtmlEscaped(),font(FontRole::Number11),item.date.isValid()?item.date.toString("MM/dd"):QString{});
            auto* metaLabel=new QLabel(meta,container);metaLabel->setObjectName("officialNoticeMeta");metaLabel->setTextFormat(Qt::RichText);column->addWidget(metaLabel);
            auto* line=new QHBoxLayout;line->setContentsMargins(0,0,0,0);line->setSpacing(6);
            auto* content=plainLabel(shownTitle,container,FontRole::Secondary14);content->setObjectName("officialNoticeTitle");content->setMinimumWidth(0);content->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);content->setAccessibleName(summary);line->addWidget(content,1);
            auto* open=new QToolButton(container);open->setText(QString(QChar(0x2197)));open->setMinimumSize(32,32);open->setAccessibleName(summary);
            const QUrl url(item.notice.url,QUrl::StrictMode);const auto source=item.source;connect(open,&QToolButton::clicked,this,[source,url]{if(OfficialNotices::allowedArticle(source,url))QDesktopServices::openUrl(url);});line->addWidget(open,0,Qt::AlignTop);
            column->addLayout(line);rows_->addWidget(container);
        }
        if(items.isEmpty()){
            // Never an endless "checking": distinguish pending, failed and empty public lists.
            bool anyAvailable=false,anyPending=false;for(const auto source:{OfficialNoticeSource::Okx,OfficialNoticeSource::Bithumb,OfficialNoticeSource::Upbit}){const auto feed=notices_->feed(source);anyAvailable|=feed.available;anyPending|=!feed.available&&(feed.loading||feed.reason==QStringLiteral("공식 공지 조회 전"));}
            const auto text=usabilityText(anyAvailable?UsabilityText::FlowNoticesNone:anyPending?UsabilityText::FlowChecking:UsabilityText::FlowNoticesFailed);
            auto* empty=plainLabel(text,this,FontRole::Secondary14);empty->setObjectName("marketFlowNoticesEmpty");setThemeStyle(empty,"QLabel{color:${text3};}");rows_->addWidget(empty);
        }
        return;
    }
    for(auto source:{OfficialNoticeSource::Okx,OfficialNoticeSource::Bithumb,OfficialNoticeSource::Upbit}){
        const auto feed=notices_->feed(source);const auto name=OfficialNotices::sourceName(source);
        sources<<usabilitySourceTime(name,0);
        if(!feed.available){statuses<<name+" · "+feed.reason;continue;}
        statuses<<name+tr(" · 수신 %1 · 최신 목록 %2건").arg(receivedText(feed.receivedAtMs)).arg(feed.notices.size());
        if(feed.notices.isEmpty())statuses<<tr("이번 응답에 공지 없음 · 위험 여부 판단 불가");
        for(const auto& notice:feed.notices.mid(0,5)){
            const auto shownTitle=usabilityPublicText(notice.title);
            const auto sourceTime=usabilitySourceTime(name,notice.publishedAtMs);
            auto* container=new QWidget(this);container->setObjectName("officialNoticeRow");auto* row=new QHBoxLayout(container);row->setContentsMargins(0,0,0,0);row->setSpacing(6);
            auto* dot=plainLabel(QString(QChar(0x2022)),container);setThemeStyle(dot,"color:${text3};");dot->setAccessibleName(tr("공개 공지"));row->addWidget(dot);
            auto* content=plainLabel(shownTitle,container);content->setObjectName("officialNoticeTitle");content->setWordWrap(false);content->setMinimumWidth(0);content->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);content->setAccessibleName(shownTitle);content->setToolTip(sourceTime);row->addWidget(content,1);
            const auto published=notice.publishedAtMs>0?tr("최초 게시 %1 KST · UTC+9").arg(QDateTime::fromMSecsSinceEpoch(notice.publishedAtMs,kst).toString("yyyy-MM-dd HH:mm")):tr("원문 게시 %1 · 시간대 미제공").arg(notice.publishedText);
            QStringList details{shownTitle,name,published};
            if(!notice.modifiedText.isEmpty())details<<tr("원문 수정 %1 · 시간대 미제공").arg(notice.modifiedText);
            if(!notice.categories.isEmpty())details<<notice.categories.join(" · ");
            details<<tr("목록 수신 %1").arg(receivedText(feed.receivedAtMs));
            auto* info=new InfoButton(container);info->setObjectName("officialNoticeInfo");info->setInformation(shownTitle,usabilityPublicText(details.join('\n')));row->addWidget(info);
            auto* open=new QToolButton(container);open->setText(QString(QChar(0x2197)));open->setMinimumSize(44,44);open->setAccessibleName(name+tr(" 공식 공지 원문: ")+shownTitle);open->setToolTip(sourceTime);
            const QUrl url(notice.url,QUrl::StrictMode);connect(open,&QToolButton::clicked,this,[source,url]{if(OfficialNotices::allowedArticle(source,url))QDesktopServices::openUrl(url);});row->addWidget(open);rows_->addWidget(container);
        }
    }
    status_->setText(KeefiFonts::mixedTextHtml(usabilityPublicText(statuses.join('\n')),FontRole::Caption12));status_->setToolTip(sources.join(" · "));
}
void OfficialNoticesView::showEvent(QShowEvent* e){QWidget::showEvent(e);if(notices_)notices_->setActive(true);refresh();}
void OfficialNoticesView::hideEvent(QHideEvent* e){QWidget::hideEvent(e);if(notices_)notices_->setActive(backgroundActive_);}
void OfficialNoticesView::setBackgroundActive(bool active){backgroundActive_=active;if(notices_)notices_->setActive(active||isVisible());}
bool OfficialNoticesView::settled()const{
    if(!notices_)return true;if(!backgroundActive_&&!isVisible())return false;
    for(const auto source:{OfficialNoticeSource::Okx,OfficialNoticeSource::Bithumb,OfficialNoticeSource::Upbit})if(notices_->feed(source).loading)return false;
    return true;
}
}
