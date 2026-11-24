#include "keefi/ChartWorkspace.h"
#include "keefi/ChartWorkspacePresentation_p.h"
#include "keefi/PlainToolTip.h"
#include "keefi/HoldingChartPanel.h"
#include "keefi/InstrumentListTree.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/ChartPublicContextSource.h"
#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/ChartDrawings.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/PortfolioController.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/FuturesTradingController.h"
#include "keefi/TradingController.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/MarketMetrics.h"
#include "keefi/TradingTransport.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/AssetValuation.h"
#include "keefi/ExtendedChartStudies.h"
#include "keefi/HoldingMarketResolver.h"
#include "keefi/TradingTypes.h"

#include <QScopeGuard>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QTimer>
#include <QToolTip>
#include <QComboBox>
#include <QAction>
#include <QMenu>
#include <QScrollBar>
#include <QTreeWidget>
#include <QBrush>
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace keefi {
using namespace chart_workspace_detail;
namespace {
constexpr qint64 PositionFreshnessMs = 180000;
}
ChartFuturesPosition projectChartFuturesPosition(const PositionRecord& position, const MarketInstrument& instrument,
    const QString& asset, const QString& provider, const QString& scope, qint64 received, qint64 asOf) {
    ChartFuturesPosition result;
    result.reason = QStringLiteral("종목 identity·원천·조회 시각을 확인하지 못한 선물 자료");
    if (!asOf) asOf = QDateTime::currentMSecsSinceEpoch();
    const auto amount = position.amount.startsWith('-') ? position.amount.mid(1) : position.amount;
    static const QRegularExpression identity("\\A[A-Z0-9]{1,80}\\z");
    static const QRegularExpression scopeText("\\A[A-Za-z0-9:_-]{1,160}\\z");
    if (provider != "binance" || !scopeText.match(scope).hasMatch() || received <= 0 || received > asOf ||
        asOf - received > PositionFreshnessMs || position.symbol != instrument.symbol || instrument.baseAsset != asset ||
        !identity.match(asset).hasMatch() || !identity.match(instrument.symbol).hasMatch() || !identity.match(instrument.quoteAsset).hasMatch() ||
        !QStringList{"LONG", "SHORT", "BOTH"}.contains(position.positionSide) || !positiveDisplayNumber(amount)) return result;
    result.entryPrice = positiveDisplayNumber(position.entryPrice) ? position.entryPrice : QString{};
    result.markPrice = positiveDisplayNumber(position.markPrice) ? position.markPrice : QString{};
    result.liquidationPrice = positiveDisplayNumber(position.liquidationPrice) ? position.liquidationPrice : QString{};
    result.quantityAvailable = true;
    result.amount = position.amount;
    result.contractSymbol = position.symbol;
    result.provider = provider;
    result.accountScope = scope;
    result.asset = asset;
    result.currency = instrument.quoteAsset;
    result.positionSide = position.positionSide;
    result.sourceUrl = "https://fapi.binance.com/fapi/v3/positionRisk";
    result.receivedAtMs = received;
    const auto pnlMagnitude = position.unrealizedPnl.startsWith('-') ? position.unrealizedPnl.mid(1) : position.unrealizedPnl;
    if (position.marginAsset == instrument.quoteAsset && decimal::valid(pnlMagnitude) &&
        (positiveDisplayNumber(pnlMagnitude) || decimal::compare(pnlMagnitude, "0") == 0)) {
        result.unrealizedPnl = position.unrealizedPnl;
        result.pnlCurrency = position.marginAsset;
    }
    // Keep the existing price/alert eligibility contract. A quantity-only
    // holding can be displayed without becoming a new alert observation.
    if (result.entryPrice.isEmpty() && result.liquidationPrice.isEmpty()) {
        result.reason = QStringLiteral("업체 진입가·청산가 정보 없음");
        return result;
    }
    result.available = true;
    result.reason.clear();
    if (!result.markPrice.isEmpty() && !result.liquidationPrice.isEmpty()) {
        const auto mark = result.markPrice.toDouble(), liquidation = result.liquidationPrice.toDouble();
        const auto distance = std::abs(liquidation - mark) / mark * 100;
        if (std::isfinite(distance)) { result.distanceAvailable = true; result.liquidationDistancePercent = distance; }
    }
    return result;
}

void ChartWorkspace::cancelContractRead() {
    const bool active = contractToken_ != 0;
    contractToken_ = 0;
    if (active && contractTransport_) contractTransport_->abort();
}
QStringList ChartWorkspace::observedFuturesSymbols(qint64 asOf) const {
    QStringList symbols;
    if (!futuresObservationActive_ || !privateEnabled_ || !portfolio_) return symbols;
    if (!asOf) asOf = QDateTime::currentMSecsSinceEpoch();
    for (const auto& connection : portfolio_->connections()) {
        if (connection.shared || connection.state != "complete" || connection.accountScope.isEmpty() || connection.account.provider != "binance" ||
            connection.account.market != "usdm_futures" || connection.receivedAtMs <= 0 || connection.receivedAtMs > asOf || asOf - connection.receivedAtMs > PositionFreshnessMs) continue;
        auto* futures = portfolio_->futures(connection.account.id); // Existing controller only; never starts a job.
        if (!futures || futures->busy()) continue;
        for (const auto& position : futures->positions()) {
            const auto amount = position.amount.startsWith('-') ? position.amount.mid(1) : position.amount;
            if (positiveDisplayNumber(amount) && !position.symbol.isEmpty() && !symbols.contains(position.symbol)) symbols.append(position.symbol);
        }
    }
    return symbols;
}
void ChartWorkspace::setFuturesObservationActive(bool active) {
    if (active == futuresObservationActive_) return;
    futuresObservationActive_ = active;
    if (!active && !isVisible()) { cancelContractRead(); contractRefresh_->stop(); }
    refreshFuturesObservations();
    emit futuresPositionObservationsChanged();
}
QList<ChartFuturesPosition> ChartWorkspace::futuresPositionObservations(qint64 asOf) const {
    QList<ChartFuturesPosition> result;
    if (!futuresObservationActive_ || !privateEnabled_ || !portfolio_) return result;
    if (!asOf) asOf = QDateTime::currentMSecsSinceEpoch();
    const bool cached = observationContractsReceivedMs_ > 0 && observationContractsReceivedMs_ <= asOf && asOf - observationContractsReceivedMs_ <= PositionFreshnessMs;
    for (const auto& connection : portfolio_->connections()) {
        if (connection.shared || connection.state != "complete" || connection.accountScope.isEmpty() || connection.account.provider != "binance" ||
            connection.account.market != "usdm_futures" || connection.receivedAtMs <= 0 || connection.receivedAtMs > asOf || asOf - connection.receivedAtMs > PositionFreshnessMs) continue;
        auto* futures = portfolio_->futures(connection.account.id);
        if (!futures || futures->busy()) continue;
        const auto instruments = futures->catalogFresh() ? futures->instruments() : QList<MarketInstrument>{};
        for (const auto& position : futures->positions()) {
            MarketInstrument matched;
            for (const auto& instrument : instruments) if (instrument.symbol == position.symbol) { matched = instrument; break; }
            if (matched.symbol.isEmpty() && cached) matched = observationContracts_.value(position.symbol);
            if (matched.symbol.isEmpty()) continue;
            const auto projected = projectChartFuturesPosition(position, matched, matched.baseAsset, connection.account.provider, connection.accountScope, connection.receivedAtMs, asOf);
            if (projected.available) result.append(projected);
        }
    }
    return result;
}
void ChartWorkspace::refreshFuturesObservations() {
    const auto symbols = observedFuturesSymbols();
    if (!symbols.isEmpty()) {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (observationContractsReceivedMs_ <= 0 || observationContractsReceivedMs_ > now || now - observationContractsReceivedMs_ > PositionFreshnessMs ||
            std::any_of(symbols.cbegin(), symbols.cend(), [this](const QString& symbol) { return !observationContracts_.contains(symbol); })) contractPending_ = true;
        contractRefresh_->start();
        requestContracts();
    } else if (!isVisible()) { cancelContractRead(); contractRefresh_->stop(); }
    emit futuresPositionObservationsChanged();
}
void ChartWorkspace::setContractTransport(TradingTransport* transport) {
    if (transport == contractTransport_) return;
    cancelContractRead();
    if (contractTransport_) disconnect(contractTransport_, nullptr, this, nullptr);
    contractTransport_ = transport;
    contract_ = {};
    contractReceivedMs_ = 0;
    observationContracts_.clear();
    observationContractsReceivedMs_ = 0;
    contractReason_ = tr("선물 종목 identity 메타데이터 조회 대기");
    contractPending_ = true;
    if (transport) {
        connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& response) {
            if (!token || token != contractToken_ || contractAsset_ != asset_ || (!isVisible() && observedFuturesSymbols().isEmpty())) return;
            contractToken_ = 0;
            contract_ = {};
            contractReceivedMs_ = 0;
            PublicDerivativesMetrics metadata;
            const auto received = QDateTime::currentMSecsSinceEpoch();
            if (MarketMetrics::applyResponse(MarketMetrics::Read::Contracts, asset_ + "USDT", response, received, &metadata) && metadata.baseAsset == asset_) {
                contract_.symbol = metadata.symbol;
                contract_.baseAsset = metadata.baseAsset;
                contract_.quoteAsset = "USDT"; // applyResponse verifies PERPETUAL and USDT quote/margin.
                contractReceivedMs_ = received;
                contractReason_.clear();
            } else contractReason_ = response.status == 429 ? tr("선물 계약 원천 요청 제한") : tr("선물 종목 identity 메타데이터 원천 검증 실패");
            observationContracts_.clear();
            observationContractsReceivedMs_ = 0;
            for (const auto& symbol : observedFuturesSymbols(received)) {
                PublicDerivativesMetrics observed;
                if (!MarketMetrics::applyResponse(MarketMetrics::Read::Contracts, symbol, response, received, &observed)) continue;
                MarketInstrument instrument;
                instrument.symbol = observed.symbol; instrument.baseAsset = observed.baseAsset; instrument.quoteAsset = "USDT";
                observationContracts_.insert(symbol, instrument);
            }
            if (!observationContracts_.isEmpty()) observationContractsReceivedMs_ = received;
            emit futuresPositionObservationsChanged();
            refreshPositions();
            requestContracts();
        });
        connect(transport, &QObject::destroyed, this, [this] {
            contractToken_ = 0;
            contract_ = {};
            contractReceivedMs_ = 0;
            observationContracts_.clear(); observationContractsReceivedMs_ = 0;
            contractReason_ = tr("선물 계약 메타데이터 연결 없음");
            emit futuresPositionObservationsChanged();
            refreshPositions();
        });
    }
    if (isVisible()) { contractRefresh_->start(); requestContracts(); refreshPositions(); }
    else refreshFuturesObservations();
}
void ChartWorkspace::requestContracts() {
    const bool chartDemand = isVisible() && chartSupported_ && !asset_.isEmpty();
    if ((!chartDemand && observedFuturesSymbols().isEmpty()) || !contractTransport_ || contractToken_ || !contractPending_ || contractPacing_->isActive()) return;
    contractPending_ = false;
    contractAsset_ = asset_;
    contractToken_ = nextContractToken_++;
    contractPacing_->start();
    contractTransport_->send(contractToken_, MarketMetrics::request(MarketMetrics::Read::Contracts));
}
void ChartWorkspace::setHoldingRows(const QList<HoldingChartRow>& rows) {
    const auto accepted=privateEnabled_?rows:QList<HoldingChartRow>{};
    if(holdingRows_==accepted)return;
    holdingRows_=accepted;if(holdingPanel_){holdingPanel_->setRows(holdingRows_);holdingPanel_->setChartIdentity(selectedChartIdentity_);}
    renderHoldingContext();
    if (input_ && browserScope_ == "holdings") filterBrowserRows();
}
void ChartWorkspace::renderHoldingContext(){
    selectedHoldingAsset_.clear();QString text;
    if(privateEnabled_)for(const auto& row:holdingRows_){
        bool matches=row.chart&&row.chart->key()==selectedChartIdentity_;
        for(const auto& candidate:row.candidates)matches=matches||candidate.key()==selectedChartIdentity_;
        if(!matches)continue;selectedHoldingAsset_=row.assetKey;QStringList names;
        for(const auto& custody:row.custodies)if(!names.contains(custody.label))names.append(custody.label);
        const auto identity=WatchlistWorkspaceList::identityFromKey(selectedChartIdentity_);
        const auto market=identity.source=="upbit"?tr("업비트"):identity.source=="binance"?tr("바이낸스"):identity.source;
        text=tr("보유: %1 · 차트: %2 %3").arg(names.join(QStringLiteral(" · ")),market,identity.code);
        if(!row.valuationMarket.isEmpty())text+=tr(" · 평가: ")+row.valuationMarket;
        if(!row.alertsEligible)text+=tr(" · 보유 관측 지연");break;
    }
    if(holdingContext_){holdingContext_->setText(text);holdingContext_->setVisible(!text.isEmpty());}
    if(assetCardAction_)assetCardAction_->setVisible(!selectedHoldingAsset_.isEmpty());
    if(holdingPanel_)holdingPanel_->setChartIdentity(selectedChartIdentity_);
}
void ChartWorkspace::setPrivateEnabled(bool enabled) {
    if (enabled == privateEnabled_) return;
    privateEnabled_ = enabled;
    if (!enabled) clearPrivate();
    else if (isVisible()) refreshPositions();
    if (enabled) refreshFuturesObservations();
    refreshWatchlists();
    refreshSelectedStudies();
}
void ChartWorkspace::clearPrivate() {
    if(browserContextMenu_)browserContextMenu_->close();
    if(auto* tree=qobject_cast<InstrumentListTree*>(watchlistRows_))tree->cancelInteractions();
    const bool wasPrivate=privateEnabled_;
    holdingRows_.clear();selectedHoldingAsset_.clear();if(holdingPanel_)holdingPanel_->clearPrivate();renderHoldingContext();
    if(wasPrivate&&gridSelector_)gridSelector_->setCurrentIndex(gridSelector_->findData(1));
    ++trendGeneration_;trendKey_.clear();trendResults_.clear();if(trendRows_)trendRows_->clear();
    QToolTip::hideText();
    privateEnabled_ = false;
    if(selectedChartIdentity_.startsWith("stock:")){
        ++selectedStudyGeneration_;selectedStudyKey_.clear();selectedStudyAppliedKey_.clear();
        if(studyFeed_)studyFeed_->clearOwner("chart-selected-studies");
        if(candles_)candles_->setSupplementalStudies({}, {}, {});
    }
    futuresObservationActive_ = false;
    observationContracts_.clear(); observationContractsReceivedMs_ = 0;
    if (!isVisible()) { cancelContractRead(); contractRefresh_->stop(); }
    if(drawings_)drawings_->clearPrivate();
    candles_->clearPrivateDrawingContext();
    candles_->clearPrivate();
    positionText_->clear();positionText_->hide();
    positionText_->setToolTip({});
    liquidationRibbon_->clear();
    liquidationRibbon_->setToolTip({});
    liquidationRibbon_->hide();
    holdingsUnavailableReason_.clear();
    delayedIdentities_.clear();
    publishedVisibleRows_.clear();
    emit visibleIdentitiesChanged({});
    clearingPrivate_ = true;
    if (watchlists_) watchlists_->clearPrivate();
    refreshWatchlists();
    clearingPrivate_ = false;
    refreshPositionPresentation();
    refreshMarketLedger();
    emit futuresPositionObservationsChanged();
}
void ChartWorkspace::refreshPositionPresentation() {
    if (!positions_ || !positionLocked_ || !positionText_) return;
    const bool hasPosition = privateEnabled_ && !positionText_->text().isEmpty();
    positions_->setVisible(detailsToggle_&&detailsToggle_->isChecked()); positionCard_->show();
    findChild<QLabel*>("chartWorkspacePositionTitle")->show();
    positionLocked_->setText(privateEnabled_ ? tr("선택 종목의 보유 내역이 없어요.") : tr("금고를 열면 보유와 평균가를 볼 수 있어요."));
    positionLocked_->setVisible(!hasPosition); positionUnlock_->setVisible(!privateEnabled_);
    findChild<QScrollArea*>("chartPositionScroll")->setVisible(hasPosition);
    positionText_->setVisible(hasPosition);
}
void ChartWorkspace::refreshHoldingsPreview() {
    if (!holdingsRows_ || (!clearingPrivate_ && (!isVisible() || (watchlists_ && instrumentPanel_->isHidden())))) return;
    const auto selected = holdingsRows_->currentItem() ? holdingsRows_->currentItem()->data(0, IdentityRole).toString() : QString{};
    const auto scroll = holdingsRows_->verticalScrollBar()->value();
    const QSignalBlocker block(holdingsRows_);
    const auto holdings = privateEnabled_ && watchlists_ ? watchlists_->holdings() : QList<WatchlistIdentity>{};
    bool reuse = holdingsRows_->topLevelItemCount() == holdings.size();
    for (int i = 0; reuse && i < holdings.size(); ++i)
        reuse = holdingsRows_->topLevelItem(i)->data(0, IdentityRole).toString() == holdings[i].key();
    if (!reuse) holdingsRows_->clear();
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto setData=[](QTreeWidgetItem* item,int column,int role,const QVariant& value){
        const auto displayed=role==Qt::ToolTipRole?QVariant(plainToolTip(value.toString())):value;
        if(item->data(column,role)!=displayed)item->setData(column,role,displayed);
    };
    for (int i = 0; i < holdings.size(); ++i) {
        const auto& identity = holdings[i];
        const auto instrument = instrumentPresentation_->label(identity);
        const auto state = watchlists_->rowState(identity);
        const bool verified = identity.market != "stock" || watchlists_->isCurrentlyVerified(identity);
        const auto badges = rowBadges(state,verified,delayedIdentities_.contains(identity.key()),now);
        QStringList badgeText;for(const auto& badge:badges)badgeText.append(movementDecoratedText(badge.text,badge.direction));
        const auto label = instrument.text() + (badgeText.isEmpty() ? QString{} : "\n" + badgeText.join(" · "));
        auto* item = reuse ? holdingsRows_->topLevelItem(i) : new QTreeWidgetItem(holdingsRows_);
        setData(item,0,IdentityRole,identity.key());
        setData(item,0,ChartInstrumentLabelRole,QStringList{instrument.name,instrument.code,{}});
        setData(item,0,WatchlistBadgesRole,QVariant::fromValue(badges));
        setData(item,1,WatchlistBadgesRole,QVariant::fromValue(badges)); // Shared footer and price baseline also invalidate this cell.
        setData(item,0,Qt::DisplayRole,label);setData(item,0,Qt::AccessibleTextRole,label);
        const auto quoteText=rowQuoteDisplay(state).text;
        setData(item,1,Qt::DisplayRole,quoteText);
        setData(item,0,Qt::ToolTipRole,label);setData(item,1,Qt::ToolTipRole,quoteText);
        setData(item,0,Qt::SizeHintRole,QSize(0,64));
        setData(item,0,Qt::ForegroundRole,verified?QVariant{}:QVariant::fromValue(QBrush(themeColors().text3)));
        static const QRegularExpression signedPercent("\\A[+-][0-9]+(?:\\.[0-9]+)?%\\z");
        bool valid=false;const auto change=state.change.chopped(1).toDouble(&valid);
        setData(item,1,ChangeDirectionRole,valid&&signedPercent.match(state.change).hasMatch()?(change>0?1:change<0?-1:0):0);
        if (identity.key()==selected && holdingsRows_->currentItem()!=item) holdingsRows_->setCurrentItem(item);
    }
    holdingsEmpty_->setText(privateEnabled_ ? tr("표시할 보유 종목이 없어요.") : tr("금고를 열면 보유 종목을 보여요."));
    holdingsEmpty_->hide(); holdingsRows_->hide();
    holdingsRows_->setFixedHeight(qMin(2, int(holdings.size()))*64+2);
    const int priceWidth=qMin(144,qMax(120,holdingsRows_->viewport()->width()/2));
    holdingsRows_->setColumnWidth(0,qMax(0,holdingsRows_->viewport()->width()-priceWidth));holdingsRows_->setColumnWidth(1,priceWidth);
    if(!reuse)holdingsRows_->doItemsLayout();
    holdingsRows_->verticalScrollBar()->setValue(scroll);
    scheduleVisibleRows();
}
void ChartWorkspace::refreshPositions() {
    const auto presentation=qScopeGuard([this]{refreshPositionPresentation();});
    if (!isVisible()) {
        refreshMarketLedger();
        return;
    }
    const auto selected = WatchlistWorkspaceList::identityFromKey(selectedChartIdentity_);
    const bool stock = selected.market == "stock";
    if (!chartSupported_ || (!stock && asset_.isEmpty())) {
        candles_->clearPrivate();
        refreshMarketLedger();
        positionText_->clear();positionText_->hide();
        liquidationRibbon_->clear();
        liquidationRibbon_->hide();
        return;
    }
    if (!privateEnabled_) {
        refreshMarketLedger();
        return;
    }
    if (!portfolio_) { clearPrivate(); return; }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (contractReceivedMs_ > now || (contractReceivedMs_ > 0 && now - contractReceivedMs_ > PositionFreshnessMs)) {
        contract_ = {};
        contractReceivedMs_ = 0;
        contractReason_ = tr("선물 종목 identity 메타데이터 만료");
    }
    const auto snapshots = portfolio_->snapshots();
    const auto fresh = [now](qint64 at) { return at > 0 && at <= now && now - at <= PositionFreshnessMs; };
    const auto evidenceMatches = [&](const AssetEvidence& evidence, const QString& scope) {
        return evidence.complete && !evidence.source.isEmpty() && evidence.scope == scope &&
            fresh(evidence.receivedAtMs) && (evidence.sourceTimeMs == 0 || fresh(evidence.sourceTimeMs));
    };
    const auto matchingSnapshot = [&](const PortfolioConnection& connection, bool requireCompleteBalance) -> const AssetSnapshot* {
        for (const auto& snapshot : snapshots)
            if (snapshot.connectionId == connection.account.id && snapshot.accountScope == connection.accountScope &&
                snapshot.provider == connection.account.provider && snapshot.market == connection.account.market &&
                (!requireCompleteBalance || snapshot.balanceComplete) && snapshot.timeMs == connection.receivedAtMs && fresh(snapshot.timeMs) &&
                evidenceMatches(snapshot.evidence, connection.accountScope)) return &snapshot;
        return nullptr;
    };
    const auto reportedPnl = [](const QString& value) {
        const auto magnitude = value.startsWith('-') ? value.mid(1) : value;
        return decimal::valid(magnitude) && (positiveDisplayNumber(magnitude) || decimal::compare(magnitude, "0") == 0);
    };
    QList<ChartPriceLine> prices;
    QStringList descriptions, ribbons;
    for (const auto& connection : portfolio_->connections()) {
        if (connection.shared || connection.state != "complete" || connection.accountScope.isEmpty() ||
            connection.receivedAtMs <= 0 || connection.receivedAtMs > now || now - connection.receivedAtMs > PositionFreshnessMs) continue;
        const auto provider = connection.account.provider;
        if (stock) {
            // Current normalized broker holdings are KR-only. A remembered US
            // symbol is not a retained quantity, average or P&L observation.
            if (selected.source != "KR" || connection.account.market != "domestic_equity" ||
                !watchlists_ || !watchlists_->isCurrentlyVerified(selected)) continue;
            // Missing cash/debt coverage keeps the total account incomplete;
            // it does not invalidate a complete individual holdings observation.
            const auto* snapshot = matchingSnapshot(connection, false);
            auto* broker = portfolio_->broker(connection.account.id); // Existing controller only; never starts a read.
            if (!snapshot || !broker) continue;
            for (const auto& value : broker->holdings().value("items").toArray()) {
                const auto row = value.toObject();
                if (row.value("symbol").toString() != selected.code) continue;
                if (provider == "toss" && (row.value("marketCountry").toString() != "KR" || row.value("currency").toString() != "KRW")) continue;
                const auto rowId = row.value("positionKey").toString(selected.code);
                const AssetComponent* component = nullptr;
                for (const auto& part : snapshot->components)
                    if (part.id == rowId && part.asset == selected.code && part.quantity == row.value("quantity").toString() &&
                        part.kind == AssetComponentKind::Holding && decimal::positive(part.quantity) &&
                        evidenceMatches(part.evidence, connection.accountScope)) { component = &part; break; }
                if (!component || component->reportedValueCurrency != "KRW") continue;
                auto title = usabilityProviderName(provider); if (title.isEmpty()) title = provider;
                QStringList values{title, usabilityText(UsabilityText::PositionQuantity).arg(groupedAmount(component->quantity))};
                const auto average = row.value("averagePurchasePrice").toString();
                if (provider == "toss" && positiveDisplayNumber(average))
                    values.append(usabilityText(UsabilityText::AveragePrice).arg(priceAmount(average), QStringLiteral("KRW")));
                if (reportedPnl(component->providerUnrealizedPnl))
                    values.append(usabilityText(UsabilityText::PositionProfitAmount).arg(priceAmount(component->providerUnrealizedPnl), QStringLiteral("KRW")));
                // Neither a stored rate with unknown units nor purchaseAmount
                // is used to invent a return percentage or average price.
                descriptions.append(values.join('\n'));
            }
            continue;
        }
        if (connection.account.market == "usdm_futures") {
            if (auto* futures = portfolio_->futures(connection.account.id)) {
                const auto instruments = futures->instruments();
                bool missingIdentity = false;
                for (const auto& position : futures->positions()) {
                    MarketInstrument matched;
                    for (const auto& instrument : instruments) if (instrument.symbol == position.symbol) { matched = instrument; break; }
                    if (matched.symbol.isEmpty() && contract_.symbol == position.symbol) matched = contract_;
                    if (matched.symbol.isEmpty()) { missingIdentity = true; continue; }
                    if (matched.baseAsset != asset_) continue;
                    const auto projected = projectChartFuturesPosition(position, matched, asset_, provider,
                        connection.accountScope, connection.receivedAtMs, now);
                    if (!projected.quantityAvailable) continue;
                    const auto title = projected.provider + " · " + projected.positionSide;
                    QStringList values{title, usabilityText(UsabilityText::PositionQuantity).arg(groupedAmount(projected.amount) + " " + projected.asset)};
                    if (!projected.entryPrice.isEmpty()) values.append(usabilityText(UsabilityText::AveragePrice).arg(priceAmount(projected.entryPrice), projected.currency));
                    if (!projected.unrealizedPnl.isEmpty()) values.append(usabilityText(UsabilityText::PositionProfitAmount).arg(priceAmount(projected.unrealizedPnl), projected.pnlCurrency));
                    if (!projected.liquidationPrice.isEmpty()) values.append(usabilityText(UsabilityText::PositionLiquidation).arg(priceAmount(projected.liquidationPrice), projected.currency));
                    descriptions.append(values.join('\n'));
                    // The view admits only the selected source's verified asset
                    // and quote currency; these reported prices are never converted.
                    if (!projected.entryPrice.isEmpty()) prices.append({provider, projected.accountScope, asset_, projected.currency,
                        projected.entryPrice, projected.sourceUrl, projected.receivedAtMs, ChartPriceKind::Entry});
                    if (!projected.liquidationPrice.isEmpty()) prices.append({provider, projected.accountScope, asset_, projected.currency,
                        projected.liquidationPrice, projected.sourceUrl, projected.receivedAtMs, ChartPriceKind::Liquidation});
                    if (projected.distanceAvailable) ribbons.append(usabilityText(UsabilityText::LiquidationDistance).arg(QString::number(projected.liquidationDistancePercent,'f',2),asset_,projected.positionSide,title));
                }
                Q_UNUSED(missingIdentity);
            }
            continue;
        }
        const auto source = provider == "upbit" ? QStringLiteral("https://api.upbit.com/v1/accounts") :
            provider == "bithumb" ? QStringLiteral("https://api.bithumb.com/v1/accounts") : QString{};
        // trading() can lazily create a manual OKX controller. Restrict this
        // accessor to the two already-running spot sources that supply averages.
        if (source.isEmpty() || connection.account.market != "spot") {
            const auto* snapshot = matchingSnapshot(connection, true);
            if (connection.account.market == "spot" && snapshot)
                for (const auto& part : snapshot->components) if (part.asset == asset_ && decimal::positive(part.quantity) &&
                    (part.kind == AssetComponentKind::Holding || part.kind == AssetComponentKind::Equity) &&
                    evidenceMatches(part.evidence, connection.accountScope)) {
                    auto title = usabilityProviderName(provider); if (title.isEmpty()) title = provider;
                    descriptions.append(title + "\n" + usabilityText(UsabilityText::PositionQuantity).arg(groupedAmount(part.quantity) + " " + asset_));
                }
            continue;
        }
        auto* controller = portfolio_->trading(connection.account.id);
        if (!controller) continue;
        for (const auto& balance : controller->balances()) {
            if (balance.asset != asset_ || !decimal::valid(balance.free) || !decimal::valid(balance.locked)) continue;
            const auto quantity = decimal::add(balance.free, balance.locked);
            if (!decimal::positive(quantity)) continue;
            const auto scope = connection.accountScope;
            QStringList values{usabilityProviderName(provider), usabilityText(UsabilityText::PositionQuantity).arg(groupedAmount(quantity) + " " + asset_)};
            if (decimal::positive(balance.providerAveragePrice) && !balance.providerAverageCurrency.isEmpty()) {
                prices.append({provider, scope, asset_, balance.providerAverageCurrency, balance.providerAveragePrice,
                    source, connection.receivedAtMs, ChartPriceKind::Average});
                values.append(usabilityText(UsabilityText::AveragePrice).arg(priceAmount(balance.providerAveragePrice), balance.providerAverageCurrency));
            }
            descriptions.append(values.join('\n'));
        }
    }
    candles_->setPriceLines(prices);
    refreshMarketLedger();
    const auto description = descriptions.join("\n\n");
    setSelectableLedgerText(positionText_,description);positionText_->setVisible(!description.isEmpty());
    const auto ribbon = ribbons.join("\n\n");
    if (liquidationRibbon_->text() != ribbon) liquidationRibbon_->setText(ribbon);
    liquidationRibbon_->setVisible(!ribbon.isEmpty());
}

} // namespace keefi
