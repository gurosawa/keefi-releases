#include "keefi/ChartWorkspace.h"
#include "keefi/ChartWorkspacePresentation_p.h"
#include "keefi/PlainToolTip.h"
#include "keefi/ChartGridView.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/ChartPublicContextSource.h"
#include "keefi/ChartIndicatorState.h"
#include "keefi/ChartStudyHelp.h"
#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/ChartMarketLedger.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/InfoButton.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/StockChartSource.h"
#include "keefi/StockChartTypes.h"
#include "keefi/ChartMarketContext.h"
#include "keefi/ChartStudies.h"
#include "keefi/ExtendedChartStudies.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MarketCandles.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/MovementDirection.h"

#include <QFutureWatcher>
#include <QPromise>
#include <QThreadPool>
#include <QDataStream>
#include <QLabel>
#include <QRegularExpression>
#include <QComboBox>
#include <QScrollBar>
#include <QTreeWidget>
#include <QByteArray>
#include <QChar>
#include <QColor>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QPointF>
#include <QPointer>
#include <QPolygonF>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTextLayout>
#include <QTreeWidgetItem>
#include <QWidget>
#include <QLayout>

#include <memory>
#include <cmath>
#include <optional>

namespace keefi {
using namespace chart_workspace_detail;
namespace {
struct SelectedStudies {QList<ExtendedTimedDirection> higher;ExtendedLowVolumeZones minute;ExtendedRelativeStrength relative;};
}
ChartStudyPreferences* ChartWorkspace::indicatorPreferences() const { return indicatorPreferences_; }
ChartStudySet ChartWorkspace::chartStudies() const { return candles_ ? candles_->chartStudies() : ChartStudySet{}; }
void ChartWorkspace::setObservationStudies(const QSet<QString>& ids){if(candles_)candles_->setObservationStudies(ids);}
void ChartWorkspace::setPublicContextSource(ChartPublicContextSource* source){publicContext_=source;if(candles_)candles_->setPublicContextSource(source);}
void ChartWorkspace::setStudyFeed(ChartStudyFeed* source){
    if(grid_)grid_->setSource(source);
    if(studyFeed_==source)return;if(studyFeed_){studyFeed_->clearOwner("chart-watch-trends");studyFeed_->clearOwner("chart-selected-studies");disconnect(studyFeed_,nullptr,this,nullptr);}studyFeed_=source;
    if(source)connect(source,&ChartStudyFeed::changed,this,[this](const QString&,qint64){refreshTrendTable();refreshSelectedStudies();});refreshTrendDemands();refreshSelectedStudies();
}
ExtendedChartStudySet ChartWorkspace::extendedStudies()const{return candles_?candles_->extendedStudies():ExtendedChartStudySet{};}
MarketCandleSeries ChartWorkspace::publicStudySeries()const{
    const auto data=selectedChartIdentity_.startsWith("stock:")?(stockSource_?stockSource_->snapshot().series:MarketCandleSeries{}):(candleModel_?candleModel_->series():MarketCandleSeries{});
    return data.identity==selectedChartIdentity_&&data.intervalMs==timeframeMs_?data:MarketCandleSeries{};
}
bool ChartWorkspace::publicStudyRealtimeAllowed()const{
    if(!publicStudySeries().available)return false;if(!selectedChartIdentity_.startsWith("stock:"))return true;
    const auto snapshot=stockSource_?stockSource_->snapshot():StockChartSnapshot{};return snapshot.realtimeAllowed&&!snapshot.delayed&&!snapshot.loading;
}
void ChartWorkspace::refreshTrendDemands(){
    if(!studyFeed_)return;QList<ChartStudyRequest> requests;
    if(trendEnabled_&&instrumentPanel_->isVisible()&&watchlists_){QList<WatchlistIdentity> ids;if(currentListId()==HoldingsList){if(privateEnabled_)ids=watchlists_->holdings();}else for(const auto& entry:watchlists_->userList(currentListId()).entries)ids.append(entry.identity);
        for(const auto& identity:ids.first(qMin(10,ids.size())))for(qint64 interval:{3600000,14400000,86400000})requests.append({identity,interval,20});}
    if(requests.isEmpty())studyFeed_->clearOwner("chart-watch-trends");else studyFeed_->setDemands("chart-watch-trends",requests);refreshTrendTable();
}
void ChartWorkspace::refreshTrendTable(){
    if(!trendRows_||!trendEnabled_||!instrumentPanel_->isVisible()||!studyFeed_)return;
    QList<WatchlistIdentity> ids;if(watchlists_){if(currentListId()==HoldingsList){if(privateEnabled_)ids=watchlists_->holdings();}else for(const auto& entry:watchlists_->userList(currentListId()).entries)ids.append(entry.identity);}ids=ids.first(qMin(10,ids.size()));
    QMap<QString,ChartStudyObservation> observations;QByteArray key;QDataStream stream(&key,QIODevice::WriteOnly);stream<<currentListId();
    const int scroll=trendRows_->verticalScrollBar()->value();trendRows_->clear();
    for(const auto& identity:ids){auto* item=new QTreeWidgetItem(trendRows_);item->setData(0,IdentityRole,identity.key());const auto label=instrumentPresentation_->label(identity);item->setText(0,label.code);item->setToolTip(0,plainToolTip(label.text()));item->setSizeHint(0,QSize(0,44));int column=1;
        for(qint64 interval:{3600000,14400000,86400000}){const auto id=identity.key()+":"+QString::number(interval);const auto observation=studyFeed_->observation(identity.key(),interval);observations.insert(id,observation);stream<<id<<observation.loading<<observation.delayed<<observation.status<<observation.series.available;
            for(const auto& row:observation.series.candles)stream<<row.startsAtMs<<row.open<<row.high<<row.low<<row.close<<row.volume;for(const auto& row:observation.timeline.rows)stream<<row.startsAtMs<<row.completesAtMs<<row.eligible<<row.connectsPrevious<<row.reason;
            const auto result=trendResults_.value(id);const bool ready=observation.series.available&&!observation.delayed&&!observation.loading&&result.state.available;
            item->setText(column,ready?(result.state.direction==IndicatorDirection::Up?tr("▲ 위"):result.state.direction==IndicatorDirection::Down?tr("▼ 아래"):tr("같음")):observation.delayed?tr("지연"):observation.loading?QStringLiteral("—"):tr("자료 없음"));
            item->setToolTip(column,observation.status+"\n"+result.state.reason+tr("\n실제 완료 봉의 SMA20 변화 · 최대 10종목"));item->setTextAlignment(column,Qt::AlignCenter);++column;
        }
    }
    for(int i=1;i<4;++i)trendRows_->setColumnWidth(i,44);trendRows_->setColumnWidth(0,qMax(48,trendRows_->viewport()->width()-132));trendRows_->verticalScrollBar()->setValue(scroll);
    if(key==trendKey_||trendPending_)return;trendKey_=key;trendPending_=true;const auto generation=++trendGeneration_;
    auto promise=std::make_shared<QPromise<QMap<QString,ExtendedTimedDirection>>>();auto* watcher=new QFutureWatcher<QMap<QString,ExtendedTimedDirection>>(this);
    connect(watcher,&QFutureWatcher<QMap<QString,ExtendedTimedDirection>>::finished,this,[this,watcher,generation]{watcher->deleteLater();trendPending_=false;if(generation==trendGeneration_&&trendEnabled_&&instrumentPanel_->isVisible())trendResults_=watcher->result();refreshTrendTable();});watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise,observations]{promise->start();QMap<QString,ExtendedTimedDirection> output;for(auto i=observations.cbegin();i!=observations.cend();++i){const auto& data=i.value();if(data.series.available&&!data.delayed&&!data.loading)output.insert(i.key(),data.stock?extendedTrend(data.series.candles,data.series.intervalMs,20,data.timeline):extendedTrend(data.series.candles,data.series.intervalMs,20));}promise->addResult(output);promise->finish();});
}
void ChartWorkspace::refreshSelectedStudies(){
    if(!studyFeed_||!candles_||!indicatorPreferences_)return;
    // Broker-only history never repopulates a locked chart from a queued result.
    // Public coin observations deliberately retain their generation on locking.
    if(selectedChartIdentity_.startsWith("stock:")&&!privateEnabled_){
        studyFeed_->clearOwner("chart-selected-studies");
        if(!selectedStudyKey_.isEmpty()||!selectedStudyAppliedKey_.isEmpty()){++selectedStudyGeneration_;selectedStudyKey_.clear();selectedStudyAppliedKey_.clear();}
        candles_->setSupplementalStudies({}, {}, {});return;
    }
    const bool higher=indicatorPreferences_->enabled("trend")||indicatorPreferences_->enabled("breakout");const bool minute=indicatorPreferences_->enabled("low_volume");const bool relative=indicatorPreferences_->enabled("relative_strength");
    const auto identity=WatchlistWorkspaceList::identityFromKey(selectedChartIdentity_);QList<ChartStudyRequest> requests;QString benchmark;
    const auto selectedSeries=publicStudySeries();if(identity.market=="coin"&&identity.source=="upbit"&&identity.code.startsWith("KRW-"))benchmark="coin:upbit:KRW-BTC";
    else if(identity.market=="coin"&&identity.source=="binance"&&selectedSeries.currency=="USDT")benchmark="coin:binance:BTCUSDT";
    if(isVisible()){if(higher)for(qint64 interval:{3600000,14400000,86400000})requests.append({identity,interval,5});if(minute)requests.append({identity,60000,5});if(relative&&!benchmark.isEmpty()){requests.append({identity,3600000,5});requests.append({WatchlistWorkspaceList::identityFromKey(benchmark),3600000,5});}}
    if(requests.isEmpty())studyFeed_->clearOwner("chart-selected-studies");else studyFeed_->setDemands("chart-selected-studies",requests);
    QMap<QString,ChartStudyObservation> observations;QByteArray key;QDataStream stream(&key,QIODevice::WriteOnly);stream<<selectedChartIdentity_<<timeframeMs_<<higher<<minute<<relative<<benchmark<<isVisible();
    for(const auto& request:requests){const auto id=request.identity.key()+":"+QString::number(request.intervalMs);if(observations.contains(id))continue;const auto data=studyFeed_->observation(request.identity.key(),request.intervalMs);observations.insert(id,data);stream<<id<<data.series.available<<data.loading<<data.delayed<<data.status;for(const auto& row:data.series.candles)stream<<row.startsAtMs<<row.open<<row.high<<row.low<<row.close<<row.volume;for(const auto& row:data.timeline.rows)stream<<row.startsAtMs<<row.completesAtMs<<row.eligible<<row.connectsPrevious;}
    if(key!=selectedStudyKey_){selectedStudyKey_=key;++selectedStudyGeneration_;candles_->setSupplementalStudies({}, {}, {});}
    if(key==selectedStudyAppliedKey_||selectedStudyPending_)return;
    if(requests.isEmpty()){selectedStudyAppliedKey_=key;return;}
    selectedStudyPending_=true;const auto generation=selectedStudyGeneration_;const auto selected=selectedChartIdentity_;
    auto promise=std::make_shared<QPromise<SelectedStudies>>();auto* watcher=new QFutureWatcher<SelectedStudies>(this);
    connect(watcher,&QFutureWatcher<SelectedStudies>::finished,this,[this,watcher,generation,key]{watcher->deleteLater();selectedStudyPending_=false;if(generation==selectedStudyGeneration_&&isVisible()){selectedStudyAppliedKey_=key;const auto result=watcher->result();candles_->setSupplementalStudies(result.higher,result.minute,result.relative);}refreshSelectedStudies();});watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise,observations,selected,benchmark,higher,minute,relative]{promise->start();SelectedStudies output;const auto get=[&](const QString& identity,qint64 interval){return observations.value(identity+":"+QString::number(interval));};const auto valid=[](const ChartStudyObservation& d){return d.series.available&&!d.loading&&!d.delayed;};
        if(higher)for(qint64 interval:{3600000,14400000,86400000}){const auto data=get(selected,interval);auto trend=valid(data)?(data.stock?extendedTrend(data.series.candles,interval,20,data.timeline):extendedTrend(data.series.candles,interval)):ExtendedTimedDirection{};trend.intervalMs=interval;if(data.stock&&!trend.state.available){const auto reason=data.timeline.recentExclusionReason(21);if(!reason.isEmpty())trend.state.reason=reason;}if(!trend.state.available&&trend.state.reason.isEmpty())trend.state.reason=data.delayed?QStringLiteral("지연"):QStringLiteral("완료 시간봉 기록 필요");output.higher.append(trend);}
        if(minute){const auto data=get(selected,60000);ExtendedChartOptions options;options.enabled.insert("low_volume");if(valid(data))output.minute=(data.stock?extendedChartStudies(data.series.candles,60000,options,data.timeline):extendedChartStudies(data.series.candles,60000,options)).lowVolumeZones;else output.minute.state.reason=QStringLiteral("실제 완료 1분 기록 필요");if(data.stock&&!output.minute.state.available){const auto reason=data.timeline.recentExclusionReason(21);if(!reason.isEmpty())output.minute.state.reason=reason;}}
        if(relative){const auto alt=get(selected,3600000),btc=get(benchmark,3600000);if(valid(alt)&&valid(btc)&&alt.series.currency==btc.series.currency)output.relative=extendedRelativeStrength(alt.series.candles,btc.series.candles,3600000,24);else output.relative.state.reason=QStringLiteral("동일 통화·시각의 BTC와 종목 기록 필요");}
        promise->addResult(output);promise->finish();});
}
void ChartWorkspace::refreshStudyPanels(){
    if(!scorecardText_||!similaritySummary_||!candles_)return;
    const auto states=candles_->indicatorStates();const auto summary=summarizeChartIndicators(states,candles_->indicatorContext(),candles_->indicatorRevision());
    indicatorSummary_->setText(summary.enabled?tr("같은 재료 기준\n▲ 위쪽 %1 · ▼ 아래쪽 %2 · 섞임 %3\n방향 없음 %4 · 자료 없음 %5").arg(summary.up).arg(summary.down).arg(summary.mix).arg(summary.info).arg(summary.none):tr("현재 방향에 반영할 지표 없음"));
    indicatorCompleteness_->setText(summary.enabled?tr("지표 %1/%2개 계산됨").arg(summary.ready).arg(summary.enabled):tr("지표에서 관측을 켤 수 있어요."));
    QStringList detail{tr("같은 OHLCV는 보수적으로 한 묶음으로 셉니다. 독립 표본이나 예측 확률이 아닙니다. 눈으로 숨긴 현재 관측 지표도 계산·집계에 포함하고 끈 지표는 제외합니다. 닮은꼴은 선택 참고인 과거 비교이므로 현재 방향과 계산된 지표 수에서 제외합니다. 거래량·선물 숫자 줄은 표시 제어여서 분모에서 제외합니다. 강한 반전 등급은 기존 반전 설명이며 새 표를 만들지 않습니다.")};
    for(const auto& group:summary.groups)detail<<group.group+" · "+indicatorDirectionText(group.direction)+(group.partial?tr(" · 일부 자료 없음"):QString{})+"\n"+group.indicatorIds.join(", ")+"\n"+group.reasons.join("\n");
    findChild<InfoButton*>("chartIndicatorSummaryInfo")->setInformation(tr("지금 상태"),detail.join("\n\n"));
    constexpr int studyIdRole=Qt::UserRole+1;
    const auto selected=indicatorRows_->currentItem()?indicatorRows_->currentItem()->data(0,studyIdRole).toString():QString{};
    const int scroll=indicatorRows_->verticalScrollBar()->value();
    QSet<QString> enabledIds;
    for(const auto& state:states)if(state.enabled&&(state.stateBearing||state.id=="strong"))enabledIds.insert(state.id);
    // Keep model indexes, selection and native accessibility identities alive
    // during public quote/study refreshes. Only membership/order changes move rows.
    QMap<QString,QTreeWidgetItem*> items;
    for(int i=indicatorRows_->topLevelItemCount()-1;i>=0;--i){
        auto* item=indicatorRows_->topLevelItem(i);const auto id=item->data(0,studyIdRole).toString();
        if(!enabledIds.contains(id))delete indicatorRows_->takeTopLevelItem(i);
        else items.insert(id,item);
    }
    int row=0;
    for(const auto& state:states)if(enabledIds.contains(state.id)){
        auto* item=items.value(state.id);
        if(!item){item=new QTreeWidgetItem;items.insert(state.id,item);}
        const auto tf=state.id=="whales"||state.id=="strong"?tr("실제 1분"):state.id=="stock_flow"?tr("일별"):state.id=="oi"||state.id=="contract"?tr("실제 1시간"):studyPeriod(timeframeMs_);
        item->setText(0,state.label+(state.hidden?tr(" · 숨김"):QString{})+" · "+tf+"\n"+state.status);
        item->setData(0,Qt::UserRole,state.label);item->setData(0,studyIdRole,state.id);item->setSizeHint(0,QSize(0,44));
        item->setToolTip(0,state.label+" · "+tf+"\n"+state.status+"\n"+state.reason);
        const int previousRow=indicatorRows_->indexOfTopLevelItem(item);
        if(previousRow!=row){
            if(previousRow>=0)indicatorRows_->takeTopLevelItem(previousRow);
            indicatorRows_->insertTopLevelItem(row,item);
        }
        ++row;
    }
    if(auto* item=items.value(selected);item&&indicatorRows_->currentItem()!=item)indicatorRows_->setCurrentItem(item);
    indicatorRows_->verticalScrollBar()->setValue(scroll);
    const auto snapshot=chartStudies();auto* selector=findChild<QComboBox*>("chartStudyScorecardSelector");const auto kind=selector?selector->currentData().toString():QString("pullback");
    const auto name=selector?selector->currentText():chartStudyName("pullback");const bool on=indicatorPreferences_&&indicatorPreferences_->enabled(kind);QStringList lines;int events=0,results=0,pending=0,overlapping=0;qint64 horizon=0;bool statistics=false;
    const auto appendSlice=[&](const QString& label,int n,int up,int down,int flat,const std::optional<double>& mean){
        if(mean&&chartScorecardStatisticsVisible(true,n)){statistics=true;lines<<label+tr(" · 평가 %1개\n위 %2 / 아래 %3 / 같음 %4\n관측 평균 %5%").arg(n).arg(up).arg(down).arg(flat).arg(QString::number(*mean,'f',2));}
        else lines<<label+tr(" · 결과가 확인된 신호 %1/20건").arg(n);
    };
    if(kind=="pullback"||kind=="whales"){
        QList<ChartHistoricalSlice> slices;
        if(kind=="pullback"){events=snapshot.entries.size();results=snapshot.scorecard.samples;pending=snapshot.scorecard.unresolvedSignals;overlapping=snapshot.scorecard.overlappingSignals;horizon=snapshot.scorecard.horizonMs;slices=chartPullbackScoreSlices(snapshot);}
        else{const auto flow=candles_->coinFlowStudies();events=flow.groups.size();results=flow.scorecard.samples;pending=flow.scorecard.unresolvedSignals;overlapping=flow.scorecard.overlappingSignals;horizon=flow.scorecard.horizonMs;slices=chartFlowScoreSlices(flow);}
        if(on)for(const auto& slice:slices)if(slice.events||slice.results)appendSlice(slice.label,slice.results,slice.up,slice.down,slice.flat,slice.meanReturnPercent);
    }else if(kind=="strong"){
        const auto strong=candles_->strongReversalSnapshot();horizon=3600000;
        for(const auto& slice:chartStrongReversalScoreSlices(strong)){
            events+=slice.events;results+=slice.results;pending+=slice.pending;overlapping+=slice.overlapping;
            if(on){appendSlice(slice.label,slice.results,slice.up,slice.down,slice.flat,slice.meanReturnPercent);
                if(slice.meanDirectionalReturnPercent&&chartScorecardStatisticsVisible(true,slice.results))lines.last()+=tr("\n방향 기준 평균 %1%").arg(*slice.meanDirectionalReturnPercent,0,'f',2);
                lines.last()+=tr("\n사건 %1 · 결과 %2 · 미해결 %3").arg(slice.events).arg(slice.results).arg(slice.pending);}
        }
        if(on)lines<<tr("최초 결정 당시 등급 고정 · 미확정은 별도 기록\n방향 기준 평균: 위쪽은 가격 변화, 아래쪽은 부호 반전");
    }else{
        const auto extended=candles_->extendedStudies();const auto score=extended.scorecards.value(kind);events=extended.studyEntries.value(kind).size();results=score.samples;pending=score.unresolvedSignals;overlapping=score.overlappingSignals;horizon=score.horizonMs;
        if(on)for(const auto& slice:score.slices){const auto direction=slice.direction==IndicatorDirection::Up?tr("위쪽 사건"):slice.direction==IndicatorDirection::Down?tr("아래쪽 사건"):tr("방향 없음");
            const auto quality=kind!="breakout"?QString{}:slice.qualityBand==-1?tr(" · 전체 품질"):slice.qualityBand==-2?tr(" · 품질 자료 없음"):slice.qualityBand==0?tr(" · 품질 0~4"):slice.qualityBand==1?tr(" · 품질 5~7"):tr(" · 품질 8~10");
            appendSlice(direction+quality,slice.samples,slice.up,slice.down,slice.flat,slice.meanReturnPercent);}
    }
    if(on)lines.prepend(tr("사건 %1 · 결과 %2 · 미해결 %3 · 겹침 %4").arg(events).arg(results).arg(pending).arg(overlapping));
    else lines={tr("지표를 켜면 해당 조건의 기록을 표시해요.")};
    const auto baseline=(kind=="whales"||kind=="strong")?tr("실제 1분"):studyPeriod(timeframeMs_);if(!horizon)horizon=(kind=="whales"||kind=="strong")?3600000:(timeframeMs_<=300000?3600000:timeframeMs_==900000?14400000:86400000);
    lines<<tr("기준 %1 · 이후 %2\n비용·슬리피지 제외").arg(baseline,studyPeriod(horizon));
    const QString sourceReason=candles_->stockUnavailableReason();
    QString emptyReason=results<ChartScorecardDisplayMinimumSamples
        ?tr("결과가 확인된 과거 신호 %1/%2건 · 같은 조건에서 %2건 이상 필요").arg(results).arg(ChartScorecardDisplayMinimumSamples)
        :tr("결과가 확인된 과거 신호 %1건 · 같은 조건에서 %2건 이상 필요").arg(results).arg(ChartScorecardDisplayMinimumSamples);
    if(kind=="strong")emptyReason=tr("등급과 방향이 같은 신호의 결과가 20건 이상 필요해요");
    if(!sourceReason.isEmpty())emptyReason=sourceReason;
    scoreEmpty_->setText(on?emptyReason:name+tr(" 꺼짐"));scoreEmpty_->setVisible(!on||!statistics);scorecardText_->setText(lines.join("\n\n"));
    QStringList parameters;
    const auto& settings = indicatorPreferences_->studySettings();
    if (kind == "ma") {
        for (const auto& average : indicatorPreferences_->movingAverages()) {
            if (!average.enabled) continue;
            const auto type = average.kind == ExtendedMaKind::Simple ? "SMA" :
                              average.kind == ExtendedMaKind::Exponential ? "EMA" : "WMA";
            parameters << QString("%1 %2").arg(type).arg(average.period);
        }
    } else if (kind == "rsi") {
        parameters << tr("RSI %1봉").arg(settings.rsiPeriod);
    } else if (kind == "macd") {
        parameters << tr("MACD 단기 %1 · 장기 %2 · 시그널 %3").arg(settings.macdFast).arg(settings.macdSlow).arg(settings.macdSignal);
    } else if (kind == "bb") {
        const ExtendedChartOptions defaults;
        parameters << tr("성적표 기준 %1봉 · 표준편차 %2배").arg(defaults.bandPeriod).arg(defaults.bandDeviations)
                   << tr("차트 표시 %1봉 · 표준편차 %2배").arg(settings.displayBandPeriod).arg(settings.displayBandDeviations);
    }
    findChild<QLabel*>("chartStudyScorecardTitle")->setText(name);findChild<InfoButton*>("chartStudyScorecardInfo")->setInformation(name+tr(" 성적표"),
        tr("기준 %1 · 이후 %2 · 결과 %3개\n사건 %4개 · 겹침 %5개 · 미도래 %6개\n%7\n지표·방향·파라미터별 해결 표본 20개 미만은 비율과 평균을 숨깁니다. 20개도 통계적 신뢰나 예측 정확도를 보장하지 않습니다. 비용·슬리피지 제외. 같은 조건의 과거 관측이며 미래 성과가 아닙니다.").arg(baseline,studyPeriod(horizon)).arg(results).arg(events).arg(overlapping).arg(pending).arg(parameters.join(" · ")));
    const auto& similar=snapshot.similarity;const bool showSimilar=indicatorPreferences_->enabled("similarity");similaritySummary_->setText(similar.available&&similar.samples>0?tr("과거 유사 구간 %1개 · 예측 아님").arg(similar.samples):similar.reason.isEmpty()?tr("과거 구간 확인 중"):similar.reason);similaritySummary_->setVisible(showSimilar);findChild<QLabel*>("chartStudySimilarityTitle")->setVisible(showSimilar);auto* similarInfo=findChild<InfoButton*>("chartStudySimilarityInfo");similarInfo->setVisible(showSimilar);
    const auto help=chartStudyHelp("similarity");QStringList comparisonDetails{tr("과거 실제 구간 %1개 · 검색 %2봉").arg(similar.samples).arg(similar.searchBars)};
    if(help)comparisonDetails<<help->brief<<help->inputs<<help->reading<<help->limitations<<help->calculation;
    similarInfo->setInformation(chartStudyName("similarity"),comparisonDetails.join("\n\n"));
    refreshPositionPresentation();refreshMarketLedger();refreshSelectedStudies();
}
void ChartWorkspace::refreshMarketLedger() {
    if (!marketLedger_ || !evidenceNotes_ || !candles_) return;
    const bool stock = selectedChartIdentity_.startsWith("stock:");
    const auto stockSnapshot = stock && stockSource_ ? stockSource_->snapshot() : StockChartSnapshot{};
    const auto series = stock ? stockSnapshot.series : candleModel_ ? candleModel_->series() : MarketCandleSeries{};
    const bool available = chartSupported_ && series.available && series.identity == selectedChartIdentity_ &&
        series.intervalMs == timeframeMs_ && !series.currency.isEmpty() && !series.candles.isEmpty();
    const auto studies = available ? (stock ? stockSnapshot.studies : candles_->chartStudies()) : ChartStudySet{};
    const int publicReference = available ? 1 : 0;
    auto row=watchlists_?watchlists_->rowState(WatchlistWorkspaceList::identityFromKey(selectedChartIdentity_)):WatchlistRowState{};
    if(row.privateData&&!privateEnabled_)row={};
    const auto quote=quotePriceParts(row.price);
    const auto display=!stock&&candleModel_?candleModel_->displayObservation():MarketCandleSeries{};
    // Archived and previously selected rows draw candles, never the price headline.
    const bool delayedPrice=!stock&&chartSupported_&&quote.first.isEmpty()&&display.delayed&&!display.fromArchive&&!display.fromMemoryCache&&
        display.identity==selectedChartIdentity_&&display.intervalMs==timeframeMs_&&
        !display.currency.isEmpty()&&!display.staleClose.isEmpty();
    const auto quoteStatus=quote.first.isEmpty()?QString{}:receivedQuoteStatus(row.quoteState);
    currentPrice_->setText(!quote.first.isEmpty()?quote.first:available?priceAmount(series.candles.last().close):
        delayedPrice?priceAmount(display.staleClose):QStringLiteral("—"));
    currentPrice_->setAccessibleName(!quoteStatus.isEmpty()?tr("마지막 받은 가격 · %1").arg(quoteStatus):
        delayedPrice?tr("마지막 완료봉 가격 · 지연"):quote.first.isEmpty()&&available?tr("최근 완료봉 종가"):usabilityText(UsabilityText::CurrentPrice));
    const auto identity=WatchlistWorkspaceList::identityFromKey(selectedChartIdentity_);
    const auto quoteCurrency=!quote.second.isEmpty()?quote.second:row.quoteCurrency;
    currentPriceUnit_->setText(!quote.first.isEmpty()&&!quoteCurrency.isEmpty()?quoteCurrency:available?series.currency:
        delayedPrice?display.currency:identity.market=="coin"&&identity.source=="upbit"?identity.code.section('-',0,0):QString{});
    static const QRegularExpression percent("\\A([+-]?[0-9]+(?:\\.[0-9]+)?)%\\z");const auto change=percent.match(row.change);
    const double changeValue=change.captured(1).toDouble();
    const bool hasChange=!quote.first.isEmpty()&&change.hasMatch()&&std::isfinite(changeValue);
    const auto direction=hasChange?movementForSign(changeValue>0?1:changeValue<0?-1:0):MovementDirection::Unknown;
    QStringList reference;
    if(!quoteStatus.isEmpty())reference.append(quoteStatus);
    else if(delayedPrice)reference.append(usabilityText(UsabilityText::Delayed));
    else if(quote.first.isEmpty()&&available)reference.append(tr("최근 완료봉"));
    if(hasChange)reference.append(movementDecoratedText(row.change,direction));
    // Words (status, '최근 완료봉') take the text face; only a bare change stays in the number face.
    const bool statusText=!quoteStatus.isEmpty()||delayedPrice||(quote.first.isEmpty()&&available);
    currentPriceReference_->setFont(KeefiFonts::font(statusText?FontRole::Caption12:FontRole::Number11));
    currentPriceReference_->setText(statusText?KeefiFonts::mixedTextHtml(reference.join(" · "),FontRole::Caption12):reference.join(" · "));
    currentPriceReference_->setVisible(!reference.isEmpty());
    const auto referenceStyle = "color:" + MovementColorPolicy::colorFor(currentPriceReference_, direction).name() + ";";
    if (currentPriceReference_->styleSheet() != referenceStyle) currentPriceReference_->setStyleSheet(referenceStyle);
    candles_->setCandleEvidenceNumber(publicReference);
    renderMarketContext();renderHoldingContext();

    // Preserve actual observation coordinates and explicit continuity. Missing
    // slots, excluded stock sessions and EMA warm-up never become graph edges.
    const auto segments = [&](const QMap<qint64, double>& values) {
        QList<QPolygonF> result;
        if (!available) return result;
        QPolygonF run;
        qint64 previous = 0;
        for (int i = 0; i < series.candles.size(); ++i) {
            const auto at = series.candles[i].startsAtMs;
            const bool timelineKnown = stockSnapshot.timeline.rows.size() == series.candles.size() &&
                stockSnapshot.timeline.rows[i].startsAtMs == at;
            const bool eligible = !stock || (timelineKnown && stockSnapshot.timeline.rows[i].eligible);
            const bool connected = previous > 0 && (stock ? timelineKnown && stockSnapshot.timeline.rows[i].connectsPrevious : at - previous == timeframeMs_);
            if (!values.contains(at) || !eligible) {
                if (!run.isEmpty()) result.append(run);
                run.clear(); previous = 0; continue;
            }
            if (!connected && !run.isEmpty()) { result.append(run); run.clear(); }
            run.append(QPointF(at, values.value(at))); previous = at;
        }
        if (!run.isEmpty()) result.append(run);
        return result;
    };
    QMap<qint64, double> volumes;
    if (available) {
        for (const auto& candle : series.candles) {
            bool volumeOk = false;
            const auto volume = candle.volume.toDouble(&volumeOk);
            if (volumeOk && std::isfinite(volume)) volumes.insert(candle.startsAtMs, volume);
        }
    }
    const auto lastAt = available ? series.candles.last().startsAtMs : 0;
    const bool currentPullback = available && studies.pullback.available && studies.lastCompletedAtMs ==
        (stock && stockSnapshot.timeline.rows.size() == series.candles.size() ? stockSnapshot.timeline.rows.last().completesAtMs : lastAt + timeframeMs_);
    marketLedger_->setRows({
        {"volume", tr("거래량"), available ? series.candles.last().volume : QString{}, {}, publicReference, segments(volumes)},
        {"pullback", chartStudyName("pullback"), currentPullback ? QString::number(studies.pullback.score) : QString{}, currentPullback ? QStringLiteral("/10") : QString{}, currentPullback ? 1 : 0, {}}
    });

    for (auto* line : marketLedger_->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly))
        if (line->layout()) line->layout()->setContentsMargins(0, 2, 0, 2);

    // Only lines accepted by the existing currency/freshness checks receive
    // private references. Internal grouping keys never enter rendered text.
    auto lines = candles_->priceLines();
    QMap<QString, int> references;
    bool renumbered = false;
    if (privateEnabled_ && !stock) for (auto& line : lines) {
        const auto key = line.provider + QChar(0x1f) + line.accountScope + QChar(0x1f) + line.sourceUrl + QChar(0x1f) + QString::number(line.receivedAtMs);
        if (!references.contains(key)) {
            const int number = references.size() + (available ? 2 : 1);
            references.insert(key, number);
        }
        const auto number = references.value(key);
        if (line.evidenceNumber != number) { line.evidenceNumber = number; renumbered = true; }
    }
    if (renumbered) candles_->setPriceLines(lines); // Display setter emits no study/source signal.
    // The source DTOs and price-line reference mapping stay internal.
    evidenceNotes_->hide();
}
void ChartWorkspace::setMarketContext(const ChartMarketContext& context) {
    if(marketContext_==context)return;
    marketContext_=context;renderMarketContext();
}
void ChartWorkspace::renderMarketContext() {
    if(!priceConversion_||!relatedMarkets_)return;
    // Context for another chart (a selection that just changed) is never shown here.
    const bool matches=!marketContext_.identityKey.isEmpty()&&marketContext_.identityKey==selectedChartIdentity_;
    const auto& colors=themeColors();
    const auto colored=[](const QString& text,const QColor& color){
        return QStringLiteral("<span style=\"color:%1\">%2</span>").arg(color.name(),KeefiFonts::mixedTextHtml(text,FontRole::Caption12));};
    QStringList first;
    if(matches&&!marketContext_.conversion.isEmpty())first.append(marketContext_.conversion);
    if(matches&&!marketContext_.premium.isEmpty())first.append(marketContext_.premium);
    const auto conversion=first.join(QStringLiteral(" · "));
    priceConversion_->setText(conversion.isEmpty()?QString{}:colored(conversion,colors.text2));
    QStringList details;
    if(matches&&!marketContext_.conversionDetail.isEmpty())details.append(marketContext_.conversionDetail);
    if(matches&&!marketContext_.premiumDetail.isEmpty())details.append(marketContext_.premiumDetail);
    priceConversion_->setToolTip(details.join(QStringLiteral("\n\n")));priceConversion_->setAccessibleName(conversion);
    priceConversion_->setProperty("contextAvailable",!conversion.isEmpty());
    // Related markets, most relevant first: as many whole items as the row has room for.
    const auto width=[&](const QString& text){
        QTextLayout layout(text,KeefiFonts::font(FontRole::Caption12));layout.setFormats(KeefiFonts::numberRuns(text));
        layout.beginLayout();auto line=layout.createLine();if(line.isValid())line.setLineWidth(1000000);layout.endLayout();
        return line.isValid()?int(std::ceil(line.naturalTextWidth())):0;};
    const int room=relatedMarkets_->width();QStringList html,plain,tips;int used=0;
    if(matches)for(const auto& item:marketContext_.related){
        const auto text=item.label+(item.value.isEmpty()?QString{}:QStringLiteral(" ")+item.value)+(item.closed?QStringLiteral(" ●"):QString{})+
            (item.change.isEmpty()?QString{}:QStringLiteral(" ")+item.change);
        const int next=used+width(text)+(plain.isEmpty()?0:width(QStringLiteral(" · ")));
        if(room>0&&next>room)break;
        used=next;plain.append(text);tips.append(item.detail);
        const auto changeColor=item.late||item.direction==MovementDirection::Unknown?colors.text3
            :item.direction==MovementDirection::Flat?colors.text2:MovementColorPolicy::colorFor(relatedMarkets_,item.direction);
        html.append(colored(item.label+(item.value.isEmpty()?QString{}:QStringLiteral(" ")+item.value),colors.text2)+
            (item.closed?colored(QStringLiteral(" ●"),colors.text3):QString{})+(item.change.isEmpty()?QString{}:colored(QStringLiteral(" ")+item.change,changeColor)));
    }
    relatedMarkets_->setText(html.join(colored(QStringLiteral(" · "),colors.text3)));
    relatedMarkets_->setToolTip(tips.join(QStringLiteral("\n")));relatedMarkets_->setAccessibleName(plain.join(QStringLiteral(", ")));
    relatedMarkets_->setProperty("contextAvailable",matches&&!marketContext_.related.isEmpty());
    QStringList allRelated;
    if(!conversion.isEmpty())allRelated<<conversion;
    allRelated<<details;
    if(matches)for(const auto& item:marketContext_.related)allRelated<<item.label+QStringLiteral(" ")+item.value+QStringLiteral(" ")+item.change+QStringLiteral("\n")+item.detail;
    if(candles_)candles_->setRelatedMarketInformation(allRelated.join(QStringLiteral("\n\n")));
    layoutToolbar();
}

} // namespace keefi
