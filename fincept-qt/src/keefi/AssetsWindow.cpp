#include "keefi/AssetsWindow.h"
#include "keefi/ProductIdentity.h"
#include "keefi/AssetsLedger.h"
#include "keefi/PortfolioController.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/BinanceEarnCollector.h"
#include "keefi/FundFlowCollector.h"
#include "keefi/PublicKrwPrices.h"
#include "keefi/Vault.h"
#include "keefi/TradingTransport.h"
#include "keefi/LocalComboBox.h"
#include "keefi/SegmentedComboBox.h"
#include "keefi/LocalTableWidget.h"
#include "keefi/UsabilityPresentation.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMap>
#include <QPushButton>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QSet>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
namespace keefi {
namespace {
QString show(const QString& s){return s.isEmpty()?QString::fromUtf8("—"):s;}
QString stamp(qint64 ms){return ms>0?QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss"):QString::fromUtf8("—");}
QLabel* label(const QString& s,QWidget* parent,const char* name){auto* l=new QLabel(s,parent);l->setTextFormat(Qt::PlainText);l->setWordWrap(true);l->setObjectName(name);return l;}
QTableWidget* table(const QStringList& headings,QWidget* p,const char* name){auto* t=new LocalTableWidget(p);t->setObjectName(name);t->setColumnCount(headings.size());t->setHorizontalHeaderLabels(headings);t->setEditTriggers(QAbstractItemView::NoEditTriggers);t->horizontalHeader()->setStretchLastSection(true);return t;}
void row(QTableWidget* t,const QStringList& cells){const int n=t->rowCount();t->insertRow(n);for(int i=0;i<cells.size();++i)t->setItem(n,i,new QTableWidgetItem(show(cells[i])));}
}
struct AssetsWindow::Private {
    AssetsWindow* q;Vault& vault;PortfolioController* portfolio=nullptr;std::unique_ptr<AssetsLedger> ownedLedger;AssetsLedger& ledger;std::unique_ptr<PublicKrwPrices> pricing;
    QTabWidget* tabs=nullptr;QLabel *summary,*feedback,*performance;QTableWidget *values,*sources,*history,*orders;QPushButton* prices;QComboBox* environment;
    QLabel *earnStatus=nullptr;QTableWidget* earnRows=nullptr;QComboBox* earnAccounts=nullptr;QPushButton* earnRefresh=nullptr;
    QWidget* coinPage=nullptr;QLabel *coinStatus=nullptr;QTableWidget *coinPeriods=nullptr,*coinLines=nullptr;QPushButton *coinRefresh=nullptr,*upbitConfirm=nullptr,*upbitWithdraw=nullptr;
    // G9-C1-a detail: both periods, every blocking reason, and the movements behind them.
    void renderCoin(){
        if(!coinPeriods)return;coinPeriods->setRowCount(0);coinLines->setRowCount(0);
        const bool unlocked=vault.isUnlocked()&&portfolio;
        auto* collector=unlocked?portfolio->flowCollector():nullptr;
        coinRefresh->setEnabled(collector&&!collector->busy());
        if(!unlocked){coinStatus->setText("금고를 열면 코인 투자 손익을 볼 수 있습니다.");upbitConfirm->hide();upbitWithdraw->hide();return;}
        const auto view=portfolio->coinPerformance();
        const bool needUpbit=!view.upbitScopesToConfirm.isEmpty();bool hasUpbit=false;
        for(const auto& s:portfolio->snapshots())if(s.provider=="upbit"&&CoinPerformance::inScope(s))hasUpbit=true;
        upbitConfirm->setVisible(hasUpbit&&needUpbit);upbitWithdraw->setVisible(hasUpbit&&!needUpbit);
        upbitConfirm->setEnabled(collector&&!collector->busy());upbitWithdraw->setEnabled(collector&&!collector->busy());
        QStringList status{"입출금을 뺀 손익이에요. 현금화한 돈이나 세금 신고 금액이 아니에요. 주식 성과는 아직 포함하지 않았어요."};
        if(!view.inScope)status<<"업비트·바이낸스(현물·USD-M) 연결이 있어야 계산할 수 있어요.";
        if(collector&&!collector->status().isEmpty())status<<collector->status();
        coinStatus->setText(status.join("\n"));
        if(!q->isVisible()||!tabs||tabs->currentWidget()!=coinPage)return;
        for(const auto* p:{&view.today,&view.sinceStart}){
            const auto money=[](const QString& v){return v.isEmpty()?QString::fromUtf8("—"):groupedAmount(v);};
            row(coinPeriods,{p->name,stamp(p->fromMs),stamp(p->toMs),money(p->opening),money(p->closing),money(p->inflow),money(p->outflow),money(p->pnl),
                p->available?QString("계산됨"):p->reasons.isEmpty()?p->reason:p->reasons.join(" · ")});
        }
        for(const auto& l:view.lines)row(coinLines,{stamp(l.timeMs),l.source,l.asset,groupedAmount(l.amount),l.direction,l.note});
    }
    Private(AssetsWindow* owner,Vault& v,TradingTransport* t,AssetsLedger* shared):q(owner),vault(v),ownedLedger(shared?nullptr:std::make_unique<AssetsLedger>(&v,QFileInfo(v.filePath()).dir().filePath("assets.keefi"))),ledger(shared?*shared:*ownedLedger),pricing(std::make_unique<PublicKrwPrices>(t)){}
    bool selected(const AssetSnapshot& s)const{const bool live=s.environment=="live"||s.environment=="mainnet";return live==(environment->currentData()=="live");}
    QList<AssetQuote> scopedQuotes(const QList<AssetSnapshot>& snapshots,bool historical=false)const{
        QSet<QString> scopes;for(const auto& s:snapshots)scopes.insert(s.accountScope);
        auto quotes=ledger.quotes();if(portfolio&&!historical)quotes.append(portfolio->quotes());quotes.erase(std::remove_if(quotes.begin(),quotes.end(),[&](const auto& q){return !scopes.contains(q.evidence.scope)&&!q.evidence.scope.startsWith("public:");}),quotes.end());return quotes;
    }
    QList<AssetSnapshot> latest()const{if(portfolio){auto snapshots=portfolio->snapshots();snapshots.erase(std::remove_if(snapshots.begin(),snapshots.end(),[this](const auto& s){return !selected(s);}),snapshots.end());return snapshots;}QMap<QString,AssetSnapshot> current;for(const auto& s:ledger.snapshots())if(selected(s)&&(!current.contains(s.accountScope)||current.value(s.accountScope).timeMs<s.timeMs))current[s.accountScope]=s;return current.values();}
    void renderEarn(){
        if(!earnRows)return;earnRows->setRowCount(0);
        const auto selectedConnection=earnAccounts->currentData().toString();earnAccounts->clear();
        auto* collector=portfolio?portfolio->earnCollector():nullptr;
        if(!vault.isUnlocked()||!ledger.isOpen()){
            earnStatus->setText("금고를 열면 암호화된 Earn 기록을 볼 수 있습니다.");earnRefresh->setEnabled(false);return;
        }
        QSet<QString> credentialGroups;
        const auto accounts=vault.accounts();auto snapshots=latest();
        // Prefer Spot for shared credentials; Spot and USD-M must not independently
        // collect the same Earn balance into two account scopes.
        std::stable_sort(snapshots.begin(),snapshots.end(),[](const auto& a,const auto& b){return a.market=="spot"&&b.market!="spot";});
        for(const auto& s:snapshots){
            if(s.provider!="binance"||s.environment!="live")continue;
            const auto a=std::find_if(accounts.cbegin(),accounts.cend(),[&](const auto& a){return a.id==s.connectionId;});
            if(a==accounts.cend()||credentialGroups.contains(a->credentialRef))continue;
            credentialGroups.insert(a->credentialRef);
            earnAccounts->addItem(QString("바이낸스 %1 · 연결 %2").arg(s.market=="spot"?"현물":"선물").arg(earnAccounts->count()+1),s.connectionId);
        }
        const auto selectedIndex=earnAccounts->findData(selectedConnection);if(selectedIndex>=0)earnAccounts->setCurrentIndex(selectedIndex);
        earnRefresh->setEnabled(collector&&!collector->busy()&&earnAccounts->count()>0);
        earnStatus->setText(collector&&!collector->status().isEmpty()?collector->status():QString("맡긴 원금·회수·보상을 별도로 기록합니다. 조회 완료는 투자 손익의 검증 완료와 다릅니다."));
        if(!q->isVisible()||!tabs||tabs->currentIndex()!=4)return;
        int shown=0;
        const auto showRecord=[&](const EarnRecord& r){
            if(shown++>=500)return;
            const QMap<QString,QString> states{{"OBSERVED","잔고 조회 기록"},{"SUCCESS","가입 완료 기록"},
                {"PAID","지급 완료 기록"},{"REPORTED","보상 지급 기록"},{"PROCESSING","처리 중"},
                {"PENDING","대기 중"},{"WAITING","대기 중"},{"CREATED","접수 기록"},
                {"FAILED","실패한 요청"},{"CANCELED","취소된 요청"},{"CANCELLED","취소된 요청"},
                {"REJECTED","거절된 요청"},{"EXPIRED","처리 기한이 지난 요청"}};
            const bool terminal=QStringList{"FAILED","CANCELED","CANCELLED","REJECTED","EXPIRED"}.contains(r.status);
            QStringList issues;for(const auto& code:r.issues){
                if(code=="STATUS_UNCONFIRMED"&&states.contains(r.status))continue;
                if(code=="PAYOUT_PENDING"&&terminal)continue;
                issues<<BinanceEarnProtocol::issueLabel(code);
            }
            QStringList route;
            if(!r.spotAmount.isEmpty()||!r.fundingAmount.isEmpty())
                route<<QString("출발: 현물 지갑 %1 · Funding 지갑 %2").arg(
                    r.spotAmount.isEmpty()?QString("—"):groupedAmount(r.spotAmount),
                    r.fundingAmount.isEmpty()?QString("—"):groupedAmount(r.fundingAmount));
            else if(!r.destinationWallet.isEmpty()){
                const QMap<QString,QString> wallets{{"SPOT","현물 지갑"},{"FUND","Funding 지갑"},
                    {"FUNDING","Funding 지갑"},{"FLEXIBLE","수시형 Earn"}};
                const auto wallet=wallets.value(r.destinationWallet);
                route<<(!wallet.isEmpty()?(earnPositions(r.source)?"회수 예정: ":"도착: ")+wallet:QString("도착 지갑 확인 필요"));
            }
            if(decimal::positive(r.lossAmount))route<<"중도 상환으로 줄어든 수익 "+groupedAmount(r.lossAmount);
            auto state=states.value(r.status,QString("거래소 처리 결과 확인 필요"));
            if(r.issues.contains("PAYOUT_PENDING")&&!terminal)state="지급 완료 확인 중";
            if(EarnTracking::recheck(r,QDateTime::currentMSecsSinceEpoch())==EarnTracking::Recheck::Expired)
                state="완료 여부 확인 필요 · 자동 재조회 기간이 지났어요";
            issues.removeDuplicates();issues.removeAll(state);
            row(earnRows,{stamp(earnPositions(r.source)?r.receivedAtMs:r.timeMs),BinanceEarnProtocol::label(r.source),r.asset,
                groupedAmount(r.amount),route.join(" · "),issues.isEmpty()?state:state+" · "+issues.join(" · ")});
        };
        for(const auto& state:ledger.earnStates()){
            if(state.connectionId!=earnAccounts->currentData().toString())continue;
            const auto current=std::find_if(snapshots.cbegin(),snapshots.cend(),[&](const auto& s){return s.connectionId==state.connectionId&&s.connectionBinding==state.binding;});
            if(current==snapshots.cend())continue;
            for(const auto& r:state.positions)showRecord(r);
            auto active=EarnTracking::active(state);std::sort(active.begin(),active.end(),[](const auto& a,const auto& b){return a.record.timeMs>b.record.timeMs;});
            for(const auto& r:active)showRecord(r.record);
        }
    }
    void render(){
        renderEarn();renderCoin();
        for(auto* t:{values,sources,history,orders})t->setRowCount(0);prices->setEnabled(vault.isUnlocked()&&!(portfolio?portfolio->pricingBusy():pricing->busy())&&ledger.isOpen());
        if(!vault.isUnlocked()||!ledger.isOpen()){summary->setText("금고를 열면 로컬에 암호화해 기록한 자산을 볼 수 있습니다.");performance->setText("기간 투자손익 · MWR · TWR: 자료 부족");return;}
        if(!q->isVisible())return;
        const auto all=latest();const auto valuation=AssetValuation::value(all,scopedQuotes(all),"KRW",QDateTime::currentMSecsSinceEpoch(),DisplayFreshnessMs,portfolio?portfolio->referenceQuotes():pricing->referenceQuotes());
        summary->setText(valuation.complete?QString("조회된 계좌 범위의 순자산 %1 KRW · %2개 계좌").arg(groupedAmount(valuation.netAssets)).arg(all.size()):
            QString("통합 순자산: 자료 부족 · 확인된 평가액 부분합 %1 KRW · %2개 계좌\n누락·오래된 잔고와 미확인 환산은 0으로 채우지 않습니다.").arg(groupedAmount(valuation.knownSubtotal)).arg(all.size()));
        QMap<QString,AssetSnapshot> scopes;for(const auto& s:all)scopes[s.accountScope]=s;
        for(const auto& v:valuation.rows){const auto s=scopes.value(v.accountScope);row(values,{s.provider+" · "+s.market,v.componentId,v.asset,groupedAmount(v.quantity),
            !v.value.isEmpty()?groupedAmount(v.value):!v.referenceValue.isEmpty()?QString("참고 %1").arg(groupedAmount(v.referenceValue)):QString{},"KRW",groupedAmount(v.providerUnrealizedPnl),v.providerPnlCurrency,v.reason});
            auto provider=usabilityProviderName(v.priceEvidence.url);if(provider.isEmpty())provider=usabilityProviderName(v.priceEvidence.source);
            values->item(values->rowCount()-1,4)->setToolTip(usabilitySourceTime(provider,v.priceEvidence.sourceTimeMs));}
        for(const auto& s:all)row(sources,{s.provider,s.market,s.environment,stamp(s.timeMs),s.balanceComplete?"조회 범위 확인":"불완전",s.coverage,usabilityProviderName(s.provider)});
        if(tabs&&tabs->currentIndex()==2){auto snapshots=ledger.snapshots();std::sort(snapshots.begin(),snapshots.end(),[](const auto&a,const auto&b){return a.timeMs>b.timeMs;});
        int shownHistory=0;for(const auto& s:snapshots){if(!selected(s)||shownHistory++>=500)continue;const auto v=AssetValuation::value({s},scopedQuotes({s},true),"KRW",s.timeMs,DisplayFreshnessMs);row(history,{stamp(s.timeMs),s.provider,s.market,groupedAmount(v.netAssets),groupedAmount(v.knownSubtotal),v.complete?"해당 계좌 범위":"자료 부족 / 수집 공백 가능"});}
        }
        if(tabs&&tabs->currentIndex()==3){auto observed=ledger.orders();std::sort(observed.begin(),observed.end(),[](const auto&a,const auto&b){return a.observedAtMs>b.observedAtMs;});
        int shownOrders=0;for(const auto& o:observed)if(scopes.contains(o.accountScope)&&shownOrders++<500)row(orders,{stamp(o.observedAtMs),o.provider,o.market,o.orderFamily,o.orderId,o.side,o.type,o.status,groupedAmount(o.quantity),groupedAmount(o.executed),priceAmount(o.price),groupedAmount(o.quoteAmount),o.currency});
        }
        performance->setText("기간 투자손익 · 외부 순입금 · MWR · TWR: 자료 부족\n현재 연결 경로는 전체 입출금·배당·권리 변동 원장을 수집하지 않습니다. 자산 증가를 수익으로 표시하지 않습니다. 주문 이력의 누적 체결 수량은 개별 체결·현금흐름으로 바꾸지 않습니다.");
    }
    bool open(bool create){QString error;if(!vault.isUnlocked()){feedback->setText("금고 잠금 해제가 필요합니다.");return false;}if(ledger.isOpen())return true;
        if(!ledger.open(create,&error)){feedback->setText(error);return false;}return true;}
    void stop(){pricing->clear();}
    void refreshPrices(){if(!open(false))return;if(portfolio)portfolio->refreshPrices();else pricing->refresh(latest());}

};
AssetsWindow::AssetsWindow(Vault& vault,QWidget* parent,TradingTransport* prices,AssetsLedger* shared):QDialog(parent),d_(std::make_unique<Private>(this,vault,prices?prices:new NetworkTradingTransport(this),shared)){
    setObjectName("assetsWindow");setWindowTitle(QStringLiteral("%1 · 자산과 성과").arg(productName()));resize(1180,800);auto* layout=new QVBoxLayout(this);
    d_->environment=new SegmentedComboBox(this);d_->environment->setObjectName("assetEnvironment");d_->environment->addItem("실전·메인넷","live");layout->addWidget(d_->environment);
    d_->summary=label({},this,"assetsSummary");layout->addWidget(d_->summary);d_->prices=new QPushButton("원화 평가가격 갱신",this);d_->prices->setObjectName("refreshAssetPrices");
    auto* priceActions=new QHBoxLayout;priceActions->addWidget(d_->prices);auto* details=new QPushButton("원화 평가 상세",this);details->setObjectName("assetPriceDetails");priceActions->addWidget(details);layout->addLayout(priceActions);
    connect(details,&QPushButton::clicked,this,[this]{
        if(!d_->vault.isUnlocked())return;
        const auto status=d_->portfolio?d_->portfolio->pricingStatus():d_->pricing->status();if(status.isEmpty())return;
        auto* box=new QMessageBox(QMessageBox::Information,"원화 평가 상태",status,QMessageBox::Ok,this);box->setObjectName("publicPriceDetails");
        box->setTextFormat(Qt::PlainText);box->setInformativeText(d_->portfolio?d_->portfolio->pricingDetails():d_->pricing->details());box->setAttribute(Qt::WA_DeleteOnClose);
        connect(&d_->vault,&Vault::aboutToLock,box,[box]{box->setText({});box->setInformativeText({});box->reject();});box->show();
    });
    layout->addWidget(label("목록은 최근 500건까지 표시합니다. 전체 기록은 암호화 파일에 보존됩니다. 자동 자산 조회와 거래 화면에서 성공한 조회를 암호화 원장에 기록합니다. 창을 열기만 해서는 금융 API를 호출하지 않습니다. 원화 환산은 출처가 확인된 직접 거래 가격만 사용하며 USDT·USDC를 USD와 같다고 가정하지 않습니다.",this,"assetsScope"));
    auto* tabs=new QTabWidget(this);d_->tabs=tabs;layout->addWidget(tabs,1);
    d_->values=table({"계좌 범위","항목","원본 자산","수량","평가액","평가 통화","거래사 평가손익","손익 통화","상태"},tabs,"assetValues");tabs->addTab(d_->values,"현재 자산");
    d_->sources=table({"서비스","시장","환경","수신 시각","완결성","조회 범위","출처"},tabs,"assetSources");tabs->addTab(d_->sources,"출처·누락 범위");
    d_->history=table({"시각","서비스","시장","순자산 KRW","부분합 KRW","상태"},tabs,"assetSnapshots");tabs->addTab(d_->history,"기록 추이");
    d_->orders=table({"관측 시각","서비스","시장","주문 종류","주문 ID","방향","유형","상태","주문 수량","누적 체결","가격","체결 총액","통화"},tabs,"assetOrderObservations");tabs->addTab(d_->orders,"주문 이력 관측");
    auto* earnPage=new QWidget(tabs);auto* earnLayout=new QVBoxLayout(earnPage);
    earnLayout->addWidget(label("Simple Earn 수시형·기간형의 원금과 보상 기록입니다. 누적·예상 보상은 원금에 다시 더하지 않습니다. 현재 이 탭의 금액은 자산 합계나 투자 손익에 자동 가산하지 않습니다. 같은 바이낸스 계정을 여러 키로 연결하면 기록이 중복될 수 있어요. 자동 재조회 기간이 지난 대기는 다시 조회해도 별도로 재확인하지 않습니다.",earnPage,"earnScope"));
    auto* earnActions=new QHBoxLayout;d_->earnAccounts=new LocalComboBox(earnPage);d_->earnAccounts->setObjectName("earnAccount");
    d_->earnRefresh=new QPushButton("Earn 기록 조회",earnPage);d_->earnRefresh->setObjectName("refreshEarnRecords");earnActions->addWidget(d_->earnAccounts);earnActions->addWidget(d_->earnRefresh);earnActions->addStretch();earnLayout->addLayout(earnActions);
    d_->earnStatus=label({},earnPage,"earnStatus");earnLayout->addWidget(d_->earnStatus);
    d_->earnRows=table({"기록 시각 / 보유 조회 시각","구분","자산","수량","이동 경로·수익 조정","상태"},earnPage,"earnRecords");earnLayout->addWidget(d_->earnRows,1);tabs->addTab(earnPage,"Earn 원금·보상");
    connect(d_->earnRefresh,&QPushButton::clicked,this,[this]{
        if(!d_->portfolio||!d_->vault.isUnlocked())return;const auto id=d_->earnAccounts->currentData().toString();
        for(const auto& s:d_->portfolio->snapshots())if(s.connectionId==id){d_->portfolio->earnCollector()->start(s);break;}
        d_->renderEarn();
    });
    connect(d_->earnAccounts,&QComboBox::activated,this,[this](int){d_->renderEarn();});
    auto* coinPage=new QWidget(tabs);d_->coinPage=coinPage;auto* coinLayout=new QVBoxLayout(coinPage);
    coinLayout->addWidget(label("업비트·바이낸스(현물·USD-M)의 입출금 기록을 모아, 기간 시작·끝 평가액에서 넣은 돈·뺀 돈을 뺀 손익을 계산합니다. 기록 시각 근처 입출금, 처리 중 입출금, 코인 입출금·지갑 이동, Earn 이동이 있는 기간은 숫자 대신 이유를 보입니다.",coinPage,"coinPerformanceDescription"));
    auto* coinActions=new QHBoxLayout;
    d_->coinRefresh=new QPushButton("입출금 기록 다시 확인",coinPage);d_->coinRefresh->setObjectName("refreshCoinFlows");
    d_->upbitConfirm=new QPushButton("업비트 포켓·스테이킹을 쓰지 않아요",coinPage);d_->upbitConfirm->setObjectName("confirmUpbitPockets");
    d_->upbitWithdraw=new QPushButton("업비트 확인 취소",coinPage);d_->upbitWithdraw->setObjectName("withdrawUpbitPockets");
    coinActions->addWidget(d_->coinRefresh);coinActions->addWidget(d_->upbitConfirm);coinActions->addWidget(d_->upbitWithdraw);coinActions->addStretch();coinLayout->addLayout(coinActions);
    d_->coinStatus=label({},coinPage,"coinPerformanceStatus");coinLayout->addWidget(d_->coinStatus);
    d_->coinPeriods=table({"구간","시작","끝","시작 평가액","끝 평가액","넣은 돈","뺀 돈","투자 손익","상태"},coinPage,"coinPerformancePeriods");coinLayout->addWidget(d_->coinPeriods);
    d_->coinLines=table({"시각","원천","자산","수량","구분","메모"},coinPage,"coinPerformanceFlows");coinLayout->addWidget(d_->coinLines,1);
    tabs->addTab(coinPage,"코인 투자 손익");
    connect(d_->coinRefresh,&QPushButton::clicked,this,[this]{if(d_->portfolio)d_->portfolio->refreshCoinFlows();d_->renderCoin();});
    connect(d_->upbitConfirm,&QPushButton::clicked,this,[this]{if(d_->portfolio)d_->portfolio->confirmUpbitPockets(true);d_->renderCoin();});
    connect(d_->upbitWithdraw,&QPushButton::clicked,this,[this]{if(d_->portfolio)d_->portfolio->confirmUpbitPockets(false);d_->renderCoin();});
    d_->performance=label({},this,"assetPerformance");layout->addWidget(d_->performance);d_->feedback=label({},this,"assetFeedback");layout->addWidget(d_->feedback);
    connect(tabs,&QTabWidget::currentChanged,this,[this]{d_->render();});connect(d_->prices,&QPushButton::clicked,this,[this]{d_->refreshPrices();});connect(d_->pricing.get(),&PublicKrwPrices::changed,this,[this]{if(d_->portfolio)return;d_->feedback->setText(d_->pricing->status());d_->render();});
    connect(d_->pricing.get(),&PublicKrwPrices::completed,this,[this](const QList<AssetQuote>& updates){
        if(d_->portfolio||!d_->vault.isUnlocked())return;
        QString error;if(!updates.isEmpty()&&!d_->ledger.append({}, {},updates,&error))d_->feedback->setText(d_->pricing->status()+" · "+error);
        d_->render();
    });
    auto* archives=new QPushButton("과거 보관 기록 열기",this);archives->setObjectName("openAssetArchive");layout->addWidget(archives);
    connect(archives,&QPushButton::clicked,this,[this]{
        if(!d_->vault.isUnlocked())return;const auto files=d_->ledger.archiveFiles();if(files.isEmpty()){d_->feedback->setText("별도로 보관한 과거 암호화 기록이 없습니다.");return;}
        bool ok=false;const auto name=QInputDialog::getItem(this,"과거 암호화 기록","보관 파일",files,0,false,&ok);if(!ok||!files.contains(name)||!d_->vault.isUnlocked())return;
        AssetsLedger archive(&d_->vault,QFileInfo(d_->ledger.filePath()).dir().filePath(name));QString error;
        if(!archive.open(false,&error)){d_->feedback->setText(error);return;}
        auto* dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(QStringLiteral("%1 · 과거 보관 기록").arg(productName()));dialog->resize(1000,650);auto* body=new QVBoxLayout(dialog);
        body->addWidget(label("선택한 보관 파일의 과거 자료입니다. 현재 잔고나 전체 순자산에 합산하지 않습니다.",dialog,"assetArchiveScope"));
        auto* records=table({"기준 시각","서비스","시장","자산","수량","원본 평가액","통화"},dialog,"archivedAssetRecords");body->addWidget(records,1);
        QList<QStringList> cells;for(const auto& s:archive.snapshots())for(const auto& c:s.components)cells.append({stamp(s.timeMs),s.provider,s.market,c.asset,groupedAmount(c.quantity),groupedAmount(c.reportedValue),c.reportedValueCurrency});
        auto data=std::make_shared<QList<QStringList>>(std::move(cells));auto page=std::make_shared<int>(0);
        auto* paging=new QHBoxLayout;auto* previous=new QPushButton("이전 200개",dialog);auto* next=new QPushButton("다음 200개",dialog);auto* pageState=label({},dialog,"archivePageState");paging->addWidget(previous);paging->addWidget(pageState);paging->addWidget(next);body->addLayout(paging);
        const auto render=[records,data,page,previous,next,pageState]{records->setRowCount(0);const int start=*page*200,end=qMin(start+200,int(data->size()));for(int i=start;i<end;++i)row(records,data->at(i));previous->setEnabled(*page>0);next->setEnabled(end<data->size());pageState->setText(QString("%1–%2 / %3개 항목").arg(data->isEmpty()?0:start+1).arg(end).arg(data->size()));};
        connect(previous,&QPushButton::clicked,dialog,[page,render]{--*page;render();});connect(next,&QPushButton::clicked,dialog,[page,render]{++*page;render();});render();
        connect(&d_->vault,&Vault::aboutToLock,dialog,[data]{data->clear();});
        auto* close=new QDialogButtonBox(QDialogButtonBox::Close,dialog);body->addWidget(close);connect(close,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
        connect(&d_->vault,&Vault::aboutToLock,dialog,[dialog,records]{records->setRowCount(0);dialog->reject();});dialog->show();
    });
    connect(d_->environment,&QComboBox::currentIndexChanged,this,[this]{d_->stop();d_->render();});
    connect(&vault,&Vault::aboutToLock,this,[this]{d_->stop();d_->ledger.lock();d_->feedback->clear();d_->render();});
    connect(&d_->ledger,&AssetsLedger::changed,this,[this]{d_->render();});connect(&d_->ledger,&AssetsLedger::locked,this,[this]{d_->render();});d_->render();
}
AssetsWindow::~AssetsWindow(){
    // The owned ledger locks during destruction. Its callbacks must not render
    // through a Private whose pricing module has already been destroyed.
    disconnect(&d_->ledger,nullptr,this,nullptr);
    disconnect(d_->pricing.get(),nullptr,this,nullptr);
    if(d_->portfolio)disconnect(d_->portfolio,nullptr,this,nullptr);
    if(d_->portfolio&&d_->portfolio->earnCollector())disconnect(d_->portfolio->earnCollector(),nullptr,this,nullptr);
    if(d_->portfolio&&d_->portfolio->flowCollector())disconnect(d_->portfolio->flowCollector(),nullptr,this,nullptr);
    d_->stop();
}
void AssetsWindow::setCurrentPortfolio(PortfolioController* portfolio){
    if(d_->portfolio||!portfolio)return;d_->stop();d_->portfolio=portfolio;
    connect(portfolio->earnCollector(),&BinanceEarnCollector::changed,this,[this]{d_->renderEarn();});
    connect(portfolio->flowCollector(),&FundFlowCollector::changed,this,[this]{d_->renderCoin();});
    connect(portfolio,&PortfolioController::coinPerformanceChanged,this,[this]{d_->renderCoin();});
    connect(portfolio,&PortfolioController::changed,this,[this]{
        d_->prices->setEnabled(d_->vault.isUnlocked()&&!d_->portfolio->pricingBusy()&&d_->ledger.isOpen());
        if(!d_->portfolio->pricingStatus().isEmpty())d_->feedback->setText(d_->portfolio->pricingStatus());
    });
    connect(portfolio,&PortfolioController::assetsChanged,this,[this]{if(!d_->tabs||d_->tabs->currentIndex()<2)d_->render();});
}
void AssetsWindow::openRecords(){d_->open(false);show();d_->render();raise();activateWindow();}
void AssetsWindow::record(const AssetCapture& capture){
    if(capture.snapshot.id.isEmpty()||!d_->vault.isUnlocked()||!d_->open(true))return;
    QList<AssetSnapshot> snapshots;bool known=false;for(const auto& s:d_->ledger.snapshots())if(s.id==capture.snapshot.id){known=true;break;}if(!known)snapshots.append(capture.snapshot);
    QString error;if(!d_->ledger.appendBatch(snapshots,{},capture.quotes,capture.orders,&error))d_->feedback->setText(error);d_->render();
}
}
