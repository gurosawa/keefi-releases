#include "keefi/ChartInstrumentPicker.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace keefi {
namespace {
using Candidate = ChartInstrumentCandidate;
QString alias(const QString& code) {
    static const QMap<QString,QString> names{{"BTC","비트코인"},{"ETH","이더리움"},{"SOL","솔라나"},
        {"HYPE","하이퍼리퀴드"},{"ZEC","지캐시"},{"005930","삼성전자 samsung"},{"000660","SK하이닉스 skhy hynix"},
        {"AAPL","애플"},{"MSFT","마이크로소프트"},{"NVDA","엔비디아"}};
    return names.value(code);
}
QStringList representativeOrder(){return {"coin:BTC","coin:ETH","coin:SOL","coin:HYPE","coin:ZEC","kr:005930","kr:000660","us:AAPL","us:MSFT","us:NVDA"};}
}
QString chartInstrumentSearchAlias(const QString& symbol) { return alias(symbol); }
QList<ChartInstrumentCandidate> chartInstrumentCandidates(PublicMarketCatalog* upbit, PublicEquityBoard* equities, ChartInstrumentCatalog* instruments) {
    QList<Candidate> next;
    if(upbit&&upbit->marketListAvailable())for(const auto& row:upbit->rows()){
        const WatchlistIdentity identity{"coin","upbit",row.market};
        if(!WatchlistWorkspaceList::validIdentity(identity)||row.market!="KRW-"+row.base||row.base.isEmpty())continue;
        next.append({identity.key(),"coin",row.base,row.koreanName.isEmpty()?row.englishName:row.koreanName,QObject::tr("업비트 · KRW")});
    }
    if(instruments)for(const auto& row:instruments->instruments("binance")){
        const WatchlistIdentity identity{"coin","binance",row.symbol};
        if(!WatchlistWorkspaceList::validIdentity(identity)||!row.tradable||row.status!="TRADING"||row.market!="spot"||
        row.quoteAsset!="USDT"||row.baseAsset.isEmpty()||row.symbol!=row.baseAsset+row.quoteAsset)continue;
        next.append({identity.key(),"coin",row.baseAsset,alias(row.baseAsset),QObject::tr("바이낸스 현물 · USDT")});
    }
    if(equities)for(const auto& section:{QString("kr"),QString("us")})for(const auto& row:equities->items(section)){
        const WatchlistIdentity identity{"stock",section.toUpper(),row.symbol};
        if(!WatchlistWorkspaceList::validIdentity(identity))continue;
        next.append({identity.key(),section,row.symbol,row.name,section=="kr"?QObject::tr("한국 · 주식"):QObject::tr("미국 · 주식")});
    }
    QHash<QString,int> counts;for(const auto& row:next)++counts[row.key];
    next.erase(std::remove_if(next.begin(),next.end(),[&](const auto& row){return counts[row.key]!=1;}),next.end());
    std::stable_sort(next.begin(),next.end(),[](const auto& a,const auto& b){
        if(a.market!=b.market)return a.market<b.market;
        if(a.symbol!=b.symbol)return a.symbol<b.symbol;
        return a.key.startsWith("coin:upbit:")&&!b.key.startsWith("coin:upbit:");
    });
    return next;
}
struct ChartInstrumentPicker::Private {
    ChartInstrumentPicker* q=nullptr;
    QPointer<PublicMarketCatalog> upbit;
    QPointer<PublicEquityBoard> equities;
    QPointer<ChartInstrumentCatalog> instruments;
    QLineEdit* search=nullptr;
    QTreeWidget* tree=nullptr;
    QLabel *status=nullptr,*feedback=nullptr;
    QPushButton *open=nullptr,*add=nullptr,*representatives=nullptr;
    QString filter="representative";
    QList<Candidate> candidates,shown;
    QStringList representativeKeys;
    bool queued=false;
    void schedule(){if(queued)return;queued=true;QTimer::singleShot(0,q,[this]{queued=false;collect();});}
    QString selectedKey()const{return tree->currentItem()?tree->currentItem()->data(0,Qt::UserRole).toString():QString{};}
    void controls(){const bool selected=!selectedKey().isEmpty();open->setEnabled(selected);add->setEnabled(selected);representatives->setEnabled(!representativeKeys.isEmpty());}
    void collect(){
        const auto next=chartInstrumentCandidates(upbit,equities,instruments);
        QStringList keys;
        for(const auto& wanted:representativeOrder())for(const auto& row:next)if(row.market+":"+row.symbol==wanted){keys.append(row.key);break;}
        const bool changed=candidates!=next||representativeKeys!=keys;candidates=next;representativeKeys=keys;
        if(changed||tree->topLevelItemCount()==0)render();else controls();
    }
    void render(){
        const auto selected=selectedKey();const auto query=search->text().trimmed();QList<Candidate> visible;
        if(filter=="representative"&&query.isEmpty()){
            for(const auto& key:representativeKeys)for(const auto& row:candidates)if(row.key==key){visible.append(row);break;}
        }else for(const auto& row:candidates){
            if(filter!="representative"&&row.market!=filter)continue;
            if(!query.isEmpty()&&!QStringList{row.key,row.symbol,row.name,row.venue,alias(row.symbol)}.join(' ').contains(query,Qt::CaseInsensitive))continue;
            visible.append(row);
        }
        if(visible!=shown){
            const QSignalBlocker block(tree);tree->clear();shown=visible;
            for(const auto& row:shown){
                const auto group=row.market=="coin"?QObject::tr("코인"):row.market=="kr"?QObject::tr("한국"):QObject::tr("미국");
                auto* item=new QTreeWidgetItem(tree,{row.name.isEmpty()?row.symbol:row.name,row.symbol,row.venue});
                item->setData(0,Qt::UserRole,row.key);item->setData(0,Qt::AccessibleTextRole,group+" · "+item->text(0)+" · "+row.symbol+" · "+row.venue);
                item->setFont(1,KeefiFonts::font(FontRole::Number11));item->setSizeHint(0,QSize(0,44));
                if(row.market!="coin")item->setToolTip(0,QObject::tr("관심 목록에는 공개 종목으로 저장해요. 차트는 증권사 연결 후 확인할 수 있어요."));
                if(row.key==selected)tree->setCurrentItem(item);
            }
            if(!tree->currentItem()&&tree->topLevelItemCount())tree->setCurrentItem(tree->topLevelItem(0));
        }
        if(shown.isEmpty())status->setText(candidates.isEmpty()?QObject::tr("아직 확인된 종목이 없어요. 공개 종목 목록을 받으면 여기에 보여요."):
            query.isEmpty()?QObject::tr("이 시장에서 확인된 종목이 없어요."):QObject::tr("일치하는 종목이 없어요. 이름이나 코드를 확인해 주세요."));
        else status->setText(filter=="representative"&&query.isEmpty()?QObject::tr("확인된 대표 종목 %1개 · 목록을 만들기 전에도 차트를 열 수 있어요.").arg(shown.size()):QObject::tr("검색 결과 %1개 · 원천에서 확인한 시장만 보여요.").arg(shown.size()));
        controls();
    }
};
ChartInstrumentPicker::ChartInstrumentPicker(PublicMarketCatalog* upbit,PublicEquityBoard* equities,ChartInstrumentCatalog* instruments,QWidget* parent)
    :QDialog(parent),d_(std::make_unique<Private>()) {
    d_->q=this;d_->upbit=upbit;d_->equities=equities;d_->instruments=instruments;
    setObjectName("chartInstrumentPicker");setWindowTitle(tr("종목 찾기 · 관심 목록 추가"));resize(760,620);setMinimumSize(540,520);
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(20,20,20,20);layout->setSpacing(12);
    auto* title=new QLabel(tr("보고 싶은 종목부터"),this);title->setFont(KeefiFonts::font(FontRole::Title17));layout->addWidget(title);
    auto* search=new QLineEdit(this);d_->search=search;search->setObjectName("chartInstrumentSearch");search->setPlaceholderText(tr("이름 또는 코드 · BTC, 삼성전자, AAPL"));search->setAccessibleName(tr("종목 이름 또는 코드 검색"));search->setMinimumHeight(44);layout->addWidget(search);
    auto* tabs=new QHBoxLayout;tabs->setSpacing(16);
    for(const auto& option:QList<QPair<QString,QString>>{{"representative",tr("대표")},{"coin",tr("코인")},{"kr",tr("한국")},{"us",tr("미국")}}){
        auto* button=new QRadioButton(option.second,this);button->setObjectName("chartInstrumentFilter_"+option.first);button->setMinimumSize(44,44);button->setChecked(option.first==d_->filter);tabs->addWidget(button);
        connect(button,&QRadioButton::toggled,this,[this,key=option.first](bool checked){if(checked){d_->filter=key;d_->render();}});
    }tabs->addStretch();layout->addLayout(tabs);
    auto* tree=new QTreeWidget(this);d_->tree=tree;tree->setObjectName("chartInstrumentResults");tree->setAccessibleName(tr("확인된 공개 종목"));tree->setColumnCount(3);tree->setHeaderLabels({tr("이름"),tr("코드"),tr("시장")});tree->setRootIsDecorated(false);tree->setSelectionMode(QAbstractItemView::SingleSelection);tree->setAlternatingRowColors(false);tree->setFont(KeefiFonts::font(FontRole::Secondary14));tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);tree->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);tree->header()->setSectionResizeMode(2,QHeaderView::ResizeToContents);tree->setMinimumHeight(140);layout->addWidget(tree,1);
    setThemeStyle(tree,"QTreeWidget{background:${bg};border:1px solid ${rule};border-radius:8px;} QTreeWidget::item:selected{background:${surface};color:${text};}");
    d_->status=new QLabel(this);d_->status->setObjectName("chartInstrumentStatus");d_->status->setWordWrap(true);d_->status->setFont(KeefiFonts::font(FontRole::Caption12));layout->addWidget(d_->status);
    auto* note=new QLabel(tr("주식 차트는 증권사 연결이 필요해요. 나스닥·코스피 지수는 현재 제공하지 않아요."),this);note->setWordWrap(true);note->setFont(KeefiFonts::font(FontRole::Caption12));layout->addWidget(note);
    d_->feedback=new QLabel(this);d_->feedback->setObjectName("chartInstrumentFeedback");d_->feedback->setWordWrap(true);d_->feedback->setTextFormat(Qt::PlainText);d_->feedback->hide();layout->addWidget(d_->feedback);
    auto* actions=new QHBoxLayout;
    const auto action=[this,actions](const QString& text,const QString& name){auto* button=new QPushButton(text,this);button->setObjectName(name);button->setMinimumSize(44,44);actions->addWidget(button);return button;};
    d_->representatives=action(tr("대표 종목으로 새 목록"),"chartInstrumentAddRepresentatives");actions->addStretch();d_->add=action(tr("관심에 추가"),"chartInstrumentAdd");d_->open=action(tr("차트 열기"),"chartInstrumentOpen");auto* close=action(tr("닫기"),"chartInstrumentClose");layout->addLayout(actions);
    connect(close,&QPushButton::clicked,this,&QDialog::reject);
    connect(search,&QLineEdit::textChanged,this,[this]{d_->render();});
    connect(tree,&QTreeWidget::currentItemChanged,this,[this]{d_->controls();});
    const auto openSelected=[this]{const auto key=d_->selectedKey();if(!key.isEmpty())emit openRequested(key);};
    connect(tree,&QTreeWidget::itemActivated,this,[openSelected](QTreeWidgetItem*,int){openSelected();});connect(d_->open,&QPushButton::clicked,this,openSelected);
    connect(d_->add,&QPushButton::clicked,this,[this]{const auto key=d_->selectedKey();if(!key.isEmpty())emit addRequested(key);});
    connect(d_->representatives,&QPushButton::clicked,this,[this]{if(!d_->representativeKeys.isEmpty())emit addRepresentativesRequested(d_->representativeKeys);});
    if(upbit){connect(upbit,&PublicMarketCatalog::changed,this,[this]{d_->schedule();});connect(upbit,&QObject::destroyed,this,[this]{d_->collect();});}
    if(equities){connect(equities,&PublicEquityBoard::changed,this,[this]{d_->schedule();});connect(equities,&QObject::destroyed,this,[this]{d_->collect();});}
    if(instruments){connect(instruments,&ChartInstrumentCatalog::changed,this,[this]{d_->schedule();});connect(instruments,&QObject::destroyed,this,[this]{d_->collect();});}
    // Catalogue expiry is independent of quote updates. Rechecking metadata
    // never fetches data and unchanged identities keep their native rows.
    auto* expiry=new QTimer(this);expiry->setInterval(1000);
    connect(expiry,&QTimer::timeout,this,[this]{if(isVisible())d_->collect();});expiry->start();
    d_->collect();search->setFocus();
}
ChartInstrumentPicker::~ChartInstrumentPicker()=default;
void ChartInstrumentPicker::setQuery(const QString& query){d_->search->setText(query);}
void ChartInstrumentPicker::setFeedback(const QString& text){d_->feedback->setText(text);d_->feedback->setVisible(!text.isEmpty());}
QStringList ChartInstrumentPicker::representativeIdentities()const{return d_->representativeKeys;}
}
